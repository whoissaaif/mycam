package io.github.whoissaaif.mycam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import android.view.OrientationEventListener
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import java.io.FileInputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.Socket
import java.io.FileOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Foreground service that owns the USB accessory link to the PC and the camera.
 * The PC decides when to stream (START/STOP) so the camera only runs while a PC app is using the webcam.
 */
class WebcamService : Service() {

    data class UiState(
        val connected: Boolean = false,
        val streaming: Boolean = false,
        val paused: Boolean = false,
        /** Video and camera-control settings (persisted). */
        val camera: CameraStreamer.Settings = CameraStreamer.Settings(),
        /** What the active camera supports; null until the camera has been opened once. */
        val cameraInfo: Protocol.CameraInfo? = null,
        val facing: Int = Protocol.FACING_BACK,
        val resolution: String = "",
        val error: String? = null,
        /** PowerManager.THERMAL_STATUS_* (0 = none); the UI warns from MODERATE up. */
        val thermal: Int = 0,
        /** Wireless mode is on (the phone can be found and connected to over Wi-Fi). */
        val wirelessOn: Boolean = false,
        /** The current link is Wi-Fi (else USB). */
        val wireless: Boolean = false,
        /** This phone's address on Wi-Fi, shown so the user can tell which network it is on. */
        val wirelessAddress: String? = null,
        /** A PC that is pairing over Wi-Fi and waiting for the user to allow it, and the code both screens show. */
        val pendingPc: String? = null,
        val pendingCode: String? = null,
        /** SystemClock.elapsedRealtime() at which the pending pairing times out (the dialog counts down to it). */
        val pendingDeadline: Long = 0L,
        /** Name of a PC whose pairing request timed out unanswered; the dialog says so until dismissed. */
        val pairingTimedOut: String? = null,
        /** The PCs this phone is paired with (they connect without asking). */
        val pairedPcs: List<PairedPc> = emptyList(),
        /** Name of the PC connected over Wi-Fi. */
        val wirelessPc: String? = null,
    )

    private lateinit var cameraThread: HandlerThread
    private lateinit var camera: Handler
    private var streamer: CameraStreamer? = null
    private var orientationListener: OrientationEventListener? = null
    private var thermalListener: Any? = null // PowerManager.OnThermalStatusChangedListener (API 29+).
    private var wakeLock: PowerManager.WakeLock? = null

    // The link to the PC: a USB accessory or (wireless mode) a TCP socket. Opened and closed on the main thread.
    private var accessory: UsbAccessory? = null
    private var pfd: ParcelFileDescriptor? = null
    private var socket: Socket? = null
    @Volatile private var output: OutputStream? = null
    @Volatile private var linked = false
    @Volatile private var linkId = 0 // Bumped per link, so a late "read ended" from an old link is ignored.
    private var readerThread: Thread? = null
    private var foreground = false

    // Wireless mode (main thread).
    private val main by lazy { Handler(mainLooper) }
    private var wireless: WirelessServer? = null
    private var wirelessOn = false
    private var wifiLock: WifiManager.WifiLock? = null
    private var handshaking = false                      // A PC is pairing or proving its pairing key.
    private var pendingAnswer: ((Boolean) -> Unit)? = null // Waiting for Allow / Don't allow on a pairing.
    private val pairedPcs by lazy { PairedPcs(getSharedPreferences(PREFS, MODE_PRIVATE)) }

    // Owned by the camera thread.
    private var wantStreaming = false
    private var paused = false // User pause (phone or PC). Persisted so a reconnect never turns the camera back on.
    private var camSettings = CameraStreamer.Settings()
    private var lastCameraInfo: Protocol.CameraInfo? = null
    private var facing = Protocol.FACING_BACK
    private var lastConfig: ByteArray? = null
    private var deviceRotation = 0
    private var framesSent = 0
    private var bytesSent = 0L
    private var lastStatsLog = 0L

    // Adaptive streaming: keep the bitrate within what the USB link can carry (camera thread only).
    private var linkBitrate = 0           // Current encoder bitrate.
    private var linkCeiling = Int.MAX_VALUE // Don't climb back above 90% of a bitrate the link fell behind at.
    private var linkCeilingSince = 0L
    private var dropUntilKey = false
    private var framesDropped = 0
    private var lastBitrateChange = 0L
    private var slowestWriteMs = 0L       // Slowest frame write since the last bitrate change.

    private val detachReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val detached = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java)
            if (accessory != null && (detached == null || detached == accessory)) {
                Log.i(TAG, "Accessory detached")
                val id = linkId
                camera.post { onLinkLost(id) }
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        cameraThread = HandlerThread("camera").apply { start() }
        camera = Handler(cameraThread.looper)
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        facing = prefs.getInt(PREF_FACING, Protocol.FACING_BACK)
        paused = prefs.getBoolean(PREF_PAUSED, false)
        camSettings = loadCameraSettings(prefs)
        _state.update { UiState(facing = facing, paused = paused, camera = camSettings, pairedPcs = pairedPcs.list()) }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val listener = PowerManager.OnThermalStatusChangedListener { status ->
                _state.update { it.copy(thermal = status) }
                camera.post { remoteLog("thermal status $status") }
            }
            getSystemService(PowerManager::class.java).addThermalStatusListener(mainExecutor, listener)
            thermalListener = listener
        }
        ContextCompat.registerReceiver(
            this, detachReceiver, IntentFilter(UsbManager.ACTION_USB_ACCESSORY_DETACHED),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        orientationListener = object : OrientationEventListener(this) {
            override fun onOrientationChanged(orientation: Int) {
                if (orientation == ORIENTATION_UNKNOWN) return
                val snapped = ((orientation + 45) / 90 % 4) * 90
                // Hysteresis: only switch when clearly past the 45 degree boundary.
                val diff = Math.abs(((orientation - deviceRotation + 540) % 360) - 180)
                if (snapped != deviceRotation && diff > 60) {
                    camera.post {
                        deviceRotation = snapped
                        sendOrientation()
                    }
                }
            }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_SET_FACING -> {
                val f = intent.getIntExtra(EXTRA_FACING, Protocol.FACING_BACK)
                camera.post { setFacing(f) }
                return START_NOT_STICKY
            }
            ACTION_COMMAND -> {
                // The phone UI uses the same commands as the PC (quality, fps, zoom, exposure, torch, focus).
                val cmd = intent.getIntExtra(EXTRA_CMD, 0)
                val arg = intent.getIntExtra(EXTRA_ARG, 0)
                camera.post { handleCommand(cmd, arg) }
                return START_NOT_STICKY
            }
            ACTION_SET_PAUSED -> {
                val p = intent.getBooleanExtra(EXTRA_PAUSED, false)
                camera.post { setPaused(p) }
                return START_NOT_STICKY
            }
            ACTION_WIRELESS -> {
                setWireless(intent.getBooleanExtra(EXTRA_ON, false))
                return START_NOT_STICKY
            }
            ACTION_FORGET_PCS -> {
                val id = intent.getStringExtra(EXTRA_PC_ID)
                if (id != null) forgetPc(id) else forgetPcs()
                return START_NOT_STICKY
            }
            ACTION_WIRELESS_ANSWER -> {
                answerPc(intent.getBooleanExtra(EXTRA_ALLOW, false))
                return START_NOT_STICKY
            }
        }

        val acc = intent?.let { IntentCompat.getParcelableExtra(it, UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java) }
        if (!goForeground()) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (acc != null && acc != accessory) openAccessory(acc)
        else if (!linked && !wirelessOn) stopSelf()
        return START_NOT_STICKY
    }

    // --- Wireless mode (IMPROVEMENTS.md 11) ----------------------------------------------------------

    /** Main thread. Turns wireless mode on (findable on Wi-Fi, accepting PCs) or off. */
    private fun setWireless(on: Boolean) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_WIRELESS, on).apply()
        if (on == wirelessOn) return
        if (on) {
            if (!goForeground()) { stopSelf(); return }
            wirelessOn = true
            wireless = WirelessServer(this) { sock, name -> main.post { offerPc(sock, name) } }.also { it.start() }
            _state.update {
                it.copy(wirelessOn = true, wirelessAddress = WirelessServer.localAddress(this), pairedPcs = pairedPcs.list())
            }
        } else {
            wirelessOn = false
            wireless?.stop()
            wireless = null
            answerPc(false)
            _state.update { it.copy(wirelessOn = false, wirelessAddress = null) }
            if (socket != null) {
                val id = linkId
                camera.post { onLinkLost(id) }
            } else if (!linked) {
                stopSelf()
            }
        }
        updateNotification()
    }

    /**
     * Main thread. A PC connected over Wi-Fi. Unless USB is in use or another PC is being set up, run the
     * handshake on its own thread: a paired PC connects straight away; a new one is paired only if the user
     * allows it after comparing the 6-digit code (PROTOCOL.md "Wireless security").
     */
    private fun offerPc(sock: Socket, discoveredName: String) {
        if (!wirelessOn || linked || handshaking) {
            // The cable wins, and only one PC at a time.
            try { sock.close() } catch (_: IOException) {}
            return
        }
        handshaking = true
        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        Thread({
            val result = WifiHandshake(sock, pairedPcs, PairedPcs.phoneId(prefs), WirelessServer.deviceName(this), ::askUser).run()
            main.post {
                handshaking = false
                if (result != null && wirelessOn && !linked) openSocket(sock, result)
                else try { sock.close() } catch (_: IOException) {}
            }
        }, "wifi-handshake").start()
        Log.i(TAG, "Wi-Fi connection from $discoveredName (${sock.inetAddress.hostAddress})")
    }

    /** Handshake thread. Shows "Pair with <PC>? Code …" and waits for Allow / Don't allow (or the timeout). */
    private fun askUser(pcName: String, code: String, pcGone: () -> Boolean): Boolean {
        val latch = java.util.concurrent.CountDownLatch(1)
        val answer = java.util.concurrent.atomic.AtomicBoolean(false)
        main.post {
            pendingAnswer = { allow -> answer.set(allow); latch.countDown() }
            val deadline = android.os.SystemClock.elapsedRealtime() + APPROVAL_TIMEOUT_MS
            _state.update { it.copy(pendingPc = pcName, pendingCode = code, pendingDeadline = deadline, pairingTimedOut = null) }
            showApprovalNotification(pcName, code)
        }
        // Wait for the answer, the timeout, or the PC hanging up (Cancel pairing on the PC).
        val giveUp = android.os.SystemClock.elapsedRealtime() + APPROVAL_TIMEOUT_MS
        var answered = false
        while (!answered && !pcGone()) {
            val left = giveUp - android.os.SystemClock.elapsedRealtime()
            if (left <= 0) break
            answered = latch.await(minOf(left, 250L), java.util.concurrent.TimeUnit.MILLISECONDS)
        }
        val cancelled = !answered && pcGone()
        main.post {
            // Unanswered: refuse, and let the dialog say "Pairing timed out" instead of vanishing (unless the
            // PC cancelled: then it just closes).
            if (!answered && pendingAnswer != null) {
                answerPc(false)
                if (!cancelled) _state.update { it.copy(pairingTimedOut = pcName) }
            }
        }
        return answered && answer.get()
    }

    /** Main thread. The user allowed or refused the PC that is pairing. */
    private fun answerPc(allow: Boolean) {
        val reply = pendingAnswer ?: return
        pendingAnswer = null
        _state.update { it.copy(pendingPc = null, pendingCode = null, pendingDeadline = 0L) }
        getSystemService(NotificationManager::class.java).cancel(APPROVAL_NOTIFICATION_ID)
        reply(allow && wirelessOn && !linked)
    }

    /** Main thread. Forgets every paired PC: they have to pair again (with a code) next time. */
    private fun forgetPcs() {
        pairedPcs.forgetAll()
        _state.update { it.copy(pairedPcs = emptyList()) }
        if (!linked && !wirelessOn) stopSelf() // Started just for this.
    }

    /** Main thread. Forgets one paired PC (A11). A PC connected right now stays connected until it leaves. */
    private fun forgetPc(id: String) {
        pairedPcs.forget(id)
        _state.update { it.copy(pairedPcs = pairedPcs.list()) }
        if (!linked && !wirelessOn) stopSelf()
    }

    private fun showApprovalNotification(name: String, code: String) {
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(APPROVAL_CHANNEL_ID, getString(R.string.channel_wireless), NotificationManager.IMPORTANCE_HIGH)
            )
        }
        fun answer(allow: Boolean, requestCode: Int) = PendingIntent.getService(
            this, requestCode, Intent(this, WebcamService::class.java).setAction(ACTION_WIRELESS_ANSWER).putExtra(EXTRA_ALLOW, allow),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val open = PendingIntent.getActivity(this, 2, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        nm.notify(
            APPROVAL_NOTIFICATION_ID,
            NotificationCompat.Builder(this, APPROVAL_CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_stat_webcam)
                .setContentTitle(getString(R.string.wireless_pair_title, name))
                .setContentText(getString(R.string.wireless_pair_text, formatCode(code)))
                .setStyle(NotificationCompat.BigTextStyle().bigText(getString(R.string.wireless_pair_text, formatCode(code))))
                .setPriority(NotificationCompat.PRIORITY_HIGH)
                .setCategory(NotificationCompat.CATEGORY_CALL)
                .setContentIntent(open)
                .setAutoCancel(true)
                .setTimeoutAfter(APPROVAL_TIMEOUT_MS)
                .addAction(0, getString(R.string.wireless_deny), answer(false, 3))
                .addAction(0, getString(R.string.wireless_allow), answer(true, 4))
                .build(),
        )
    }

    /** Main thread. Uses a paired, encrypted Wi-Fi connection as the link. */
    private fun openSocket(sock: Socket, secure: WifiHandshake.Result) {
        closeLink()
        try {
            sock.tcpNoDelay = true
            // A small send buffer keeps delay low: a big one hides a slow network from the adaptive bitrate
            // (writes return at once while seconds of video queue up behind them).
            sock.sendBufferSize = 128 * 1024
        } catch (_: IOException) {}
        socket = sock
        startLink(secure.output, secure.input, wirelessPc = secure.pcName)
        if (secure.newlyPaired) _state.update { it.copy(pairedPcs = pairedPcs.list()) }
        wifiLock = getSystemService(WifiManager::class.java)?.createWifiLock(
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            else @Suppress("DEPRECATION") WifiManager.WIFI_MODE_FULL_HIGH_PERF,
            "MyCam:stream",
        )?.apply { setReferenceCounted(false); acquire() }
        Log.i(TAG, "Wi-Fi link to ${secure.pcName} (${sock.inetAddress.hostAddress}), encrypted")
    }


    private fun goForeground(): Boolean {
        if (ContextCompat.checkSelfPermission(this, android.Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            Log.w(TAG, "Camera permission missing; cannot start")
            return false
        }
        val nm = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW)
            )
        }
        val notification = buildNotification()
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
            foreground = true
            true
        } catch (e: Exception) {
            Log.e(TAG, "startForeground failed", e)
            false
        }
    }

    /** Ongoing notification with a Pause / Resume action, so the camera can be paused from the shade. */
    private fun buildNotification(): Notification {
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )
        val toggle = PendingIntent.getService(
            this, 1,
            Intent(this, WebcamService::class.java).setAction(ACTION_SET_PAUSED).putExtra(EXTRA_PAUSED, !paused),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_webcam)
            .setContentTitle(getString(when { paused -> R.string.notification_title_paused; !linked && wirelessOn -> R.string.notification_title_wireless; else -> R.string.notification_title }))
            .setContentText(getString(when { paused -> R.string.notification_text_paused; !linked && wirelessOn -> R.string.notification_text_wireless; else -> R.string.notification_text }))
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .addAction(
                if (paused) R.drawable.ic_action_resume else R.drawable.ic_action_pause,
                getString(if (paused) R.string.action_resume else R.string.action_pause),
                toggle,
            )
            .build()
    }

    private fun updateNotification() {
        if (!foreground) return
        getSystemService(NotificationManager::class.java).notify(NOTIFICATION_ID, buildNotification())
    }

    private fun openAccessory(acc: UsbAccessory) {
        // The cable wins over Wi-Fi: drop a wireless link (and any PC waiting for approval).
        answerPc(false)
        closeLink()
        val usb = getSystemService(UsbManager::class.java)
        val fd = try { usb.openAccessory(acc) } catch (e: SecurityException) { null }
        if (fd == null) {
            _state.update { it.copy(error = getString(R.string.error_open_accessory)) }
            if (!wirelessOn) stopSelf()
            return
        }
        accessory = acc
        pfd = fd
        startLink(FileOutputStream(fd.fileDescriptor), FileInputStream(fd.fileDescriptor), wirelessPc = null)
    }

    /** Main thread. Common start of a USB or Wi-Fi link: reader thread, sensors, wake lock, UI state. */
    private fun startLink(out: OutputStream, input: InputStream, wirelessPc: String?) {
        val id = ++linkId
        output = out
        linked = true
        _state.update { it.copy(connected = true, error = null, wireless = wirelessPc != null, wirelessPc = wirelessPc) }
        orientationListener?.takeIf { it.canDetectOrientation() }?.enable()
        acquireWakeLock()
        updateNotification()
        readerThread = Thread({ readLoop(input, id) }, if (wirelessPc != null) "wifi-reader" else "accessory-reader").apply { start() }
    }

    private fun readLoop(input: InputStream, id: Int) {
        // AOA reads must use a buffer of at least 16 KiB or some kernels fail the transfer.
        val buf = ByteArray(16384)
        val parser = Protocol.CommandParser()
        try {
            while (!Thread.currentThread().isInterrupted) {
                val n = input.read(buf)
                if (n < 0) break
                for (c in parser.feed(buf, n)) camera.post { handleCommand(c.cmd, c.arg) }
            }
        } catch (e: IOException) {
            Log.i(TAG, "Link read ended: ${e.message}")
        }
        // The PC side went away (companion closed, cable pulled or Wi-Fi lost).
        camera.post { onLinkLost(id) }
    }

    /**
     * Camera thread. The link [id] ended: turn the camera off and close it. The service stays up while
     * wireless mode is on (a PC may connect again); otherwise it stops, as before.
     */
    private fun onLinkLost(id: Int) {
        if (id != linkId || !linked) return
        wantStreaming = false
        stopStreamer()
        main.post {
            if (id != linkId) return@post
            closeLink()
            if (!wirelessOn) stopSelf()
        }
    }

    private fun handleCommand(cmd: Int, arg: Int) {
        remoteLog("command $cmd (arg $arg)")
        when (cmd) {
            Protocol.CMD_HELLO -> {
                send(Protocol.TYPE_HELLO, 0, 0, byteArrayOf(0, Protocol.VERSION.toByte()))
                sendState()
                sendOrientation()
                sendCameraInfo()
            }
            Protocol.CMD_START -> {
                wantStreaming = true
                if (paused) sendState() // Stay off; the PC shows its "Camera paused" picture.
                else if (streamer == null) startStreamer()
                else {
                    lastConfig?.let { send(Protocol.TYPE_CONFIG, 0, 0, it) }
                    streamer?.requestKeyFrame()
                }
            }
            Protocol.CMD_STOP -> {
                wantStreaming = false
                stopStreamer()
            }
            Protocol.CMD_KEYFRAME -> {
                lastConfig?.let { send(Protocol.TYPE_CONFIG, 0, 0, it) }
                streamer?.requestKeyFrame()
            }
            Protocol.CMD_SET_FACING -> setFacing(arg)
            Protocol.CMD_PAUSE -> setPaused(true)
            Protocol.CMD_RESUME -> setPaused(false)
            Protocol.CMD_SET_QUALITY -> changeVideo(camSettings.copy(quality = arg.coerceIn(Protocol.QUALITY_720P, Protocol.QUALITY_4K)))
            Protocol.CMD_SET_FPS -> changeVideo(camSettings.copy(fps = if (arg >= 120) 120 else if (arg >= 60) 60 else 30))
            Protocol.CMD_SET_ZOOM -> changeControls(camSettings.copy(zoom = arg / 10f))
            Protocol.CMD_SET_EXPOSURE -> changeControls(camSettings.copy(ev = arg.toByte().toInt()))
            Protocol.CMD_SET_TORCH -> changeControls(camSettings.copy(torch = arg != 0))
            Protocol.CMD_SET_FOCUS -> changeControls(camSettings.copy(focusLocked = arg != 0))
        }
    }

    /** Camera thread. Pausing turns the camera fully off; resuming restarts it only if the PC still wants video. */
    private fun setPaused(p: Boolean) {
        if (p != paused) {
            paused = p
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_PAUSED, p).apply()
            _state.update { it.copy(paused = p) }
            updateNotification()
            remoteLog(if (p) "paused" else "resumed")
        }
        when {
            p && streamer != null -> stopStreamer() // Reports STATE_PAUSED.
            !p && wantStreaming && streamer == null -> startStreamer()
            else -> sendState()
        }
    }

    /** Quality or frame rate changed: save, and restart the camera if it is running. */
    private fun changeVideo(s: CameraStreamer.Settings) {
        if (s == camSettings) return
        camSettings = s
        saveCameraSettings(getSharedPreferences(PREFS, MODE_PRIVATE), s)
        _state.update { it.copy(camera = s) }
        if (streamer != null) {
            stopStreamer()
            startStreamer()
        } else {
            sendCameraInfo()
        }
    }

    /** Zoom, exposure, torch or focus changed: save, and apply live without restarting. */
    private fun changeControls(s: CameraStreamer.Settings) {
        camSettings = s
        saveCameraSettings(getSharedPreferences(PREFS, MODE_PRIVATE), s)
        _state.update { it.copy(camera = s) }
        streamer?.updateControls(s) ?: sendCameraInfo()
    }

    /** Tells the PC the current settings (and, once known, what the camera supports). */
    private fun sendCameraInfo() {
        val c = camSettings
        val info = lastCameraInfo?.copy(
            quality = c.quality, fps = c.fps, zoomX100 = (c.zoom * 100).toInt().coerceIn(lastCameraInfo!!.zoomMinX100, lastCameraInfo!!.zoomMaxX100),
            ev = c.ev.coerceIn(lastCameraInfo!!.evMin, lastCameraInfo!!.evMax),
            flags = (lastCameraInfo!!.flags and (Protocol.CAM_TORCH_ON or Protocol.CAM_FOCUS_LOCKED).inv()) or
                (if (c.torch && lastCameraInfo!!.flags and Protocol.CAM_TORCH_AVAILABLE != 0) Protocol.CAM_TORCH_ON else 0) or
                (if (c.focusLocked && lastCameraInfo!!.flags and Protocol.CAM_HAS_AUTOFOCUS != 0) Protocol.CAM_FOCUS_LOCKED else 0),
        ) ?: Protocol.CameraInfo(c.quality, c.fps, (c.zoom * 100).toInt(), 100, 100, c.ev, 0, 0, 0, 0, 0, 0, 0)
        send(Protocol.TYPE_CAMERA, 0, 0, info.encode())
    }

    private fun setFacing(f: Int) {
        if (f != Protocol.FACING_BACK && f != Protocol.FACING_FRONT) return
        facing = f
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(PREF_FACING, f).apply()
        _state.update { it.copy(facing = f) }
        if (streamer != null) {
            stopStreamer()
            startStreamer()
        } else {
            sendState()
        }
    }

    private fun startStreamer() {
        if (!linked) return
        remoteLog("starting ${if (facing == Protocol.FACING_FRONT) "front" else "back"} camera")
        framesSent = 0
        bytesSent = 0L
        val s = CameraStreamer(this, camera, facing, camSettings, object : CameraStreamer.Listener {
            override fun onLog(message: String) = remoteLog(message)

            override fun onCameraInfo(info: Protocol.CameraInfo) {
                lastCameraInfo = info
                _state.update { it.copy(cameraInfo = info) }
                sendCameraInfo()
            }

            override fun onConfig(width: Int, height: Int, sensorOrientation: Int, facing: Int, csd: ByteArray) {
                val payload = ByteBuffer.allocate(7 + csd.size).order(ByteOrder.BIG_ENDIAN)
                    .putShort(width.toShort()).putShort(height.toShort())
                    .putShort(sensorOrientation.toShort()).put(facing.toByte()).put(csd).array()
                lastConfig = payload
                send(Protocol.TYPE_CONFIG, 0, 0, payload)
                _state.update { it.copy(streaming = true, resolution = "${width}×$height", error = null) }
                sendState()
            }

            override fun onFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean) {
                // Behind: frames are skipped until the next key frame, so the backlog never builds up
                // (H.264 can't drop single frames without breaking the picture until the next key frame).
                if (dropUntilKey) {
                    if (!keyFrame) { framesDropped++; return }
                    dropUntilKey = false
                }
                val t0 = SystemClock.elapsedRealtime()
                send(Protocol.TYPE_FRAME, if (keyFrame) Protocol.FLAG_KEYFRAME else 0, ptsUs, data)
                adaptToLink(SystemClock.elapsedRealtime() - t0, keyFrame)
                framesSent++
                bytesSent += data.size
                val now = SystemClock.elapsedRealtime()
                if (framesSent == 1 || now - lastStatsLog > 5000) {
                    remoteLog("sent $framesSent frames (${bytesSent / 1024} KB), dropped $framesDropped, bitrate ${linkBitrate / 1_000_000} Mbps")
                    lastStatsLog = now
                }
            }

            override fun onError(message: String) {
                Log.e(TAG, message)
                remoteLog("error: $message")
                stopStreamer()
                _state.update { it.copy(error = message) }
                send(Protocol.TYPE_STATE, 0, 0, byteArrayOf(Protocol.STATE_ERROR.toByte()))
            }
        })
        streamer = s
        linkBitrate = 0 // Set from the streamer's target on the first frame.
        linkCeiling = Int.MAX_VALUE
        dropUntilKey = false
        framesDropped = 0
        slowestWriteMs = 0
        lastBitrateChange = SystemClock.elapsedRealtime()
        s.start()
    }

    private fun stopStreamer() {
        streamer?.stop()
        streamer = null
        lastConfig = null
        _state.update { it.copy(streaming = false, resolution = "") }
        sendState()
    }

    /**
     * A frame write that takes longer than its time slot means the USB link (the PC reads as fast as it can)
     * is carrying less than the encoder produces. Then: skip to the next key frame and lower the bitrate by
     * a quarter. When writes stay fast for 5 s, raise the bitrate back towards the mode's target, but not
     * above 90% of a bitrate the link already fell behind at (climbing back there just repeats the stall).
     * The ceiling is never below twice the floor and expires after 30 s: a stall can come from a busy PC
     * rather than the link, and a ceiling at the floor would pin the stream at the worst quality (ISSUES.md 1).
     * The arithmetic is in Long: 4K bitrates times 115 overflow Int, which once sent the encoder a
     * negative bitrate and made the picture collapse every ~30 s.
     */
    private fun adaptToLink(writeMs: Long, keyFrame: Boolean) {
        val s = streamer ?: return
        if (linkBitrate == 0) linkBitrate = s.targetBitrate
        val slot = s.frameIntervalMs.toLong()
        val now = SystemClock.elapsedRealtime()
        if (linkCeiling != Int.MAX_VALUE && now - linkCeilingSince > CEILING_EXPIRY_MS) linkCeiling = Int.MAX_VALUE
        // Key frames are several times larger, so they get more room before counting as "behind".
        if (writeMs > slot * (if (keyFrame) 4 else 2) && now - lastBitrateChange > 1000) {
            linkCeiling = minOf(linkCeiling, (linkBitrate.toLong() * 9 / 10).toInt()).coerceAtLeast(MIN_BITRATE * 2)
            linkCeilingSince = now
            linkBitrate = (linkBitrate.toLong() * 3 / 4).toInt().coerceAtLeast(MIN_BITRATE)
            s.setBitrate(linkBitrate)
            dropUntilKey = true
            s.requestKeyFrame()
            remoteLog("link behind (frame write took $writeMs ms): bitrate now ${linkBitrate / 1000} kbps, skipping to next key frame")
            lastBitrateChange = now
            slowestWriteMs = 0
            return
        }
        slowestWriteMs = maxOf(slowestWriteMs, if (keyFrame) writeMs / 4 else writeMs)
        val limit = minOf(s.targetBitrate, linkCeiling)
        if (now - lastBitrateChange > 5000 && linkBitrate < limit) {
            if (slowestWriteMs < slot / 2) {
                linkBitrate = (linkBitrate.toLong() * 115 / 100).coerceAtMost(limit.toLong()).toInt()
                s.setBitrate(linkBitrate)
                remoteLog("link has room: bitrate now ${linkBitrate / 1000} kbps")
            }
            lastBitrateChange = now
            slowestWriteMs = 0
        }
    }

    private fun sendState() {
        val st = when {
            paused -> Protocol.STATE_PAUSED
            streamer != null -> Protocol.STATE_STREAMING
            else -> Protocol.STATE_IDLE
        }
        send(Protocol.TYPE_STATE, 0, 0, byteArrayOf(st.toByte(), facing.toByte()))
    }

    private fun sendOrientation() {
        send(Protocol.TYPE_ORIENT, 0, 0, byteArrayOf((deviceRotation shr 8).toByte(), deviceRotation.toByte()))
    }

    /** Logs locally and forwards the text to the PC log (mycam.log). Camera thread only. */
    private fun remoteLog(message: String) {
        Log.i(TAG, message)
        send(Protocol.TYPE_LOG, 0, 0, message.toByteArray())
    }

    /** Called on the camera thread only, so writes are naturally serialized. */
    private fun send(type: Int, flags: Int, ptsUs: Long, payload: ByteArray) {
        val out = output ?: return
        try {
            out.write(Protocol.packet(type, flags, ptsUs, payload))
        } catch (e: IOException) {
            Log.i(TAG, "Accessory write failed: ${e.message}")
            output = null
            stopStreamer()
        }
    }

    private fun acquireWakeLock() {
        if (wakeLock == null) {
            wakeLock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MyCam:link")
                .apply { setReferenceCounted(false); acquire() }
        }
    }

    /** Closes the current link (USB or Wi-Fi), if any. */
    private fun closeLink() {
        linked = false
        readerThread?.interrupt()
        readerThread = null
        try { pfd?.close() } catch (_: IOException) {}
        try { socket?.close() } catch (_: IOException) {}
        pfd = null
        socket = null
        output = null
        accessory = null
        wifiLock?.release()
        wifiLock = null
        orientationListener?.disable()
        _state.update { it.copy(connected = false, streaming = false, resolution = "", wireless = false, wirelessPc = null) }
        updateNotification()
    }

    override fun onDestroy() {
        unregisterReceiver(detachReceiver)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            (thermalListener as? PowerManager.OnThermalStatusChangedListener)
                ?.let { getSystemService(PowerManager::class.java).removeThermalStatusListener(it) }
        }
        orientationListener?.disable()
        camera.post {
            streamer?.stop()
            streamer = null
        }
        cameraThread.quitSafely()
        wireless?.stop()
        wireless = null
        pendingAnswer?.invoke(false) // Unblocks a pairing that is waiting for the user.
        pendingAnswer = null
        closeLink()
        wakeLock?.release()
        wakeLock = null
        _state.update { UiState(facing = facing, paused = paused, camera = camSettings, pairedPcs = pairedPcs.list()) }
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        private const val TAG = "WebcamService"
        private const val CHANNEL_ID = "webcam"
        private const val NOTIFICATION_ID = 1
        private const val MIN_BITRATE = 2_000_000
        private const val CEILING_EXPIRY_MS = 30_000L
        const val PREFS = "mycam"
        const val PREF_FACING = "facing"
        const val ACTION_SET_FACING = "io.github.whoissaaif.mycam.SET_FACING"
        const val EXTRA_FACING = "facing"
        const val PREF_PAUSED = "paused"
        const val ACTION_COMMAND = "io.github.whoissaaif.mycam.COMMAND"
        const val EXTRA_CMD = "cmd"
        const val EXTRA_ARG = "arg"
        const val PREF_WIRELESS = "wireless"
        const val ACTION_WIRELESS = "io.github.whoissaaif.mycam.WIRELESS"
        const val EXTRA_ON = "on"
        const val ACTION_WIRELESS_ANSWER = "io.github.whoissaaif.mycam.WIRELESS_ANSWER"
        const val EXTRA_ALLOW = "allow"
        const val ACTION_FORGET_PCS = "io.github.whoissaaif.mycam.FORGET_PCS"
        /** With ACTION_FORGET_PCS: forget only this PC (PairedPc.id); without it, forget them all. */
        const val EXTRA_PC_ID = "pc_id"
        /** Dim the screen automatically while streaming (Settings > Phone). */
        const val PREF_AUTO_DIM = "auto_dim"
        /** Set after the first successful connection: the first-run cards stop showing. */
        const val PREF_FIRST_RUN_DONE = "first_run_done"

        /** "554294" -> "554 294", easier to compare between screens. */
        fun formatCode(code: String) = if (code.length == 6) code.substring(0, 3) + " " + code.substring(3) else code
        private const val APPROVAL_CHANNEL_ID = "wireless"
        private const val APPROVAL_NOTIFICATION_ID = 2
        private const val APPROVAL_TIMEOUT_MS = 60_000L

        fun loadCameraSettings(prefs: android.content.SharedPreferences) = CameraStreamer.Settings(
            quality = prefs.getInt("quality", Protocol.QUALITY_1080P),
            fps = prefs.getInt("fps", 30),
            zoom = prefs.getFloat("zoom", 1f),
            ev = prefs.getInt("ev", 0),
            // Torch and focus lock reset on each connection: a torch left on would surprise people.
        )

        fun saveCameraSettings(prefs: android.content.SharedPreferences, s: CameraStreamer.Settings) {
            prefs.edit().putInt("quality", s.quality).putInt("fps", s.fps).putFloat("zoom", s.zoom).putInt("ev", s.ev).apply()
        }

        /** Shows saved camera settings while no PC is connected. */
        fun showIdleCamera(s: CameraStreamer.Settings) {
            _state.update { if (it.connected) it else it.copy(camera = s) }
        }
        const val ACTION_SET_PAUSED = "io.github.whoissaaif.mycam.SET_PAUSED"
        const val EXTRA_PAUSED = "paused"

        private val _state = MutableStateFlow(UiState())
        val state: StateFlow<UiState> = _state.asStateFlow()

        /** Shows the saved camera choice while no PC is connected. */
        fun showIdleFacing(facing: Int) {
            _state.update { if (it.connected) it else it.copy(facing = facing) }
        }

        /** Shows the saved pause choice while no PC is connected. */
        fun showIdlePaused(paused: Boolean) {
            _state.update { if (it.connected) it else it.copy(paused = paused) }
        }

        /** Shows the paired PCs before the service runs (Settings > Paired PCs). */
        fun showPairedPcs(pcs: List<PairedPc>) {
            _state.update { it.copy(pairedPcs = pcs) }
        }

        /** The user closed the "Pairing timed out" dialog. */
        fun dismissPairingTimedOut() {
            _state.update { it.copy(pairingTimedOut = null) }
        }

        /** How long a pairing request waits for Allow / Don't allow; the dialog counts this down. */
        const val PAIRING_TIMEOUT_MS = APPROVAL_TIMEOUT_MS
    }
}
