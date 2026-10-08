package com.example.mycam

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
        val facing: Int = Protocol.FACING_BACK,
        val resolution: String = "",
        val error: String? = null,
    )

    private lateinit var cameraThread: HandlerThread
    private lateinit var camera: Handler
    private var streamer: CameraStreamer? = null
    private var orientationListener: OrientationEventListener? = null
    private var wakeLock: PowerManager.WakeLock? = null

    private var accessory: UsbAccessory? = null
    private var pfd: ParcelFileDescriptor? = null
    private var output: FileOutputStream? = null
    private var readerThread: Thread? = null

    // Owned by the camera thread.
    private var wantStreaming = false
    private var facing = Protocol.FACING_BACK
    private var lastConfig: ByteArray? = null
    private var deviceRotation = 0
    private var framesSent = 0
    private var bytesSent = 0L
    private var lastStatsLog = 0L

    private val detachReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val detached = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java)
            if (detached == null || detached == accessory) {
                Log.i(TAG, "Accessory detached")
                stopSelf()
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        cameraThread = HandlerThread("camera").apply { start() }
        camera = Handler(cameraThread.looper)
        facing = getSharedPreferences(PREFS, MODE_PRIVATE).getInt(PREF_FACING, Protocol.FACING_BACK)
        _state.update { UiState(facing = facing) }
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
        }

        val acc = intent?.let { IntentCompat.getParcelableExtra(it, UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java) }
        if (!goForeground()) {
            stopSelf()
            return START_NOT_STICKY
        }
        if (acc != null && acc != accessory) openAccessory(acc)
        else if (accessory == null) stopSelf()
        return START_NOT_STICKY
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
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE,
        )
        val notification: Notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_webcam)
            .setContentTitle(getString(R.string.notification_title))
            .setContentText(getString(R.string.notification_text))
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
            } else {
                startForeground(NOTIFICATION_ID, notification)
            }
            true
        } catch (e: Exception) {
            Log.e(TAG, "startForeground failed", e)
            false
        }
    }

    private fun openAccessory(acc: UsbAccessory) {
        closeAccessory()
        val usb = getSystemService(UsbManager::class.java)
        val fd = try { usb.openAccessory(acc) } catch (e: SecurityException) { null }
        if (fd == null) {
            _state.update { it.copy(error = getString(R.string.error_open_accessory)) }
            stopSelf()
            return
        }
        accessory = acc
        pfd = fd
        output = FileOutputStream(fd.fileDescriptor)
        val input = FileInputStream(fd.fileDescriptor)
        _state.update { it.copy(connected = true, error = null) }
        orientationListener?.takeIf { it.canDetectOrientation() }?.enable()
        acquireWakeLock()

        readerThread = Thread({ readLoop(input) }, "accessory-reader").apply { start() }
    }

    private fun readLoop(input: FileInputStream) {
        // AOA reads must use a buffer of at least 16 KiB or some kernels fail the transfer.
        val buf = ByteArray(16384)
        val pending = ByteBuffer.allocate(4096).order(ByteOrder.BIG_ENDIAN)
        try {
            while (!Thread.currentThread().isInterrupted) {
                val n = input.read(buf)
                if (n < 0) break
                if (n > pending.remaining()) pending.clear() // Garbage; resync.
                pending.put(buf, 0, n)
                pending.flip()
                while (pending.remaining() >= Protocol.COMMAND_SIZE) {
                    pending.mark()
                    if (pending.int != Protocol.COMMAND_MAGIC) {
                        pending.reset(); pending.get() // Skip one byte and try to resync.
                        continue
                    }
                    val cmd = pending.get().toInt() and 0xFF
                    val arg = pending.get().toInt() and 0xFF
                    pending.short
                    camera.post { handleCommand(cmd, arg) }
                }
                pending.compact()
            }
        } catch (e: IOException) {
            Log.i(TAG, "Accessory read ended: ${e.message}")
        }
        camera.post {
            // The PC side went away (companion closed or cable pulled).
            if (accessory != null) stopSelf()
        }
    }

    private fun handleCommand(cmd: Int, arg: Int) {
        remoteLog("command $cmd (arg $arg)")
        when (cmd) {
            Protocol.CMD_HELLO -> {
                send(Protocol.TYPE_HELLO, 0, 0, byteArrayOf(0, Protocol.VERSION.toByte()))
                sendState()
                sendOrientation()
            }
            Protocol.CMD_START -> {
                wantStreaming = true
                if (streamer == null) startStreamer()
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
        }
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
        if (accessory == null) return
        remoteLog("starting ${if (facing == Protocol.FACING_FRONT) "front" else "back"} camera")
        framesSent = 0
        bytesSent = 0L
        val s = CameraStreamer(this, camera, facing, object : CameraStreamer.Listener {
            override fun onLog(message: String) = remoteLog(message)

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
                send(Protocol.TYPE_FRAME, if (keyFrame) Protocol.FLAG_KEYFRAME else 0, ptsUs, data)
                framesSent++
                bytesSent += data.size
                val now = SystemClock.elapsedRealtime()
                if (framesSent == 1 || now - lastStatsLog > 5000) {
                    remoteLog("sent $framesSent frames, ${bytesSent / 1024} KB so far")
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
        s.start()
    }

    private fun stopStreamer() {
        streamer?.stop()
        streamer = null
        lastConfig = null
        _state.update { it.copy(streaming = false, resolution = "") }
        sendState()
    }

    private fun sendState() {
        val st = if (streamer != null) Protocol.STATE_STREAMING else Protocol.STATE_IDLE
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

    private fun closeAccessory() {
        readerThread?.interrupt()
        readerThread = null
        try { pfd?.close() } catch (_: IOException) {}
        pfd = null
        output = null
        accessory = null
    }

    override fun onDestroy() {
        unregisterReceiver(detachReceiver)
        orientationListener?.disable()
        camera.post {
            streamer?.stop()
            streamer = null
        }
        cameraThread.quitSafely()
        closeAccessory()
        wakeLock?.release()
        wakeLock = null
        _state.update { UiState(facing = facing) }
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    companion object {
        private const val TAG = "WebcamService"
        private const val CHANNEL_ID = "webcam"
        private const val NOTIFICATION_ID = 1
        const val PREFS = "mycam"
        const val PREF_FACING = "facing"
        const val ACTION_SET_FACING = "com.example.mycam.SET_FACING"
        const val EXTRA_FACING = "facing"

        private val _state = MutableStateFlow(UiState())
        val state: StateFlow<UiState> = _state.asStateFlow()

        /** Shows the saved camera choice while no PC is connected. */
        fun showIdleFacing(facing: Int) {
            _state.update { if (it.connected) it else it.copy(facing = facing) }
        }
    }
}
