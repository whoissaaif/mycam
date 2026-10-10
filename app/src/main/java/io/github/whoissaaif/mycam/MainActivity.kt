package io.github.whoissaaif.mycam

import android.Manifest
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.hardware.usb.UsbAccessory
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.view.MotionEvent
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import androidx.core.content.edit
import io.github.whoissaaif.mycam.ui.DimScreen
import io.github.whoissaaif.mycam.ui.FirstRunScreen
import io.github.whoissaaif.mycam.ui.ScreenActions
import io.github.whoissaaif.mycam.ui.WebcamScreen
import io.github.whoissaaif.mycam.ui.theme.MycamTheme
import kotlinx.coroutines.delay

class MainActivity : ComponentActivity() {

    /** Accessory waiting for the camera permission before the service can start. */
    private var pendingAccessory: UsbAccessory? = null

    /** Whether we already asked for permissions this launch (avoid re-asking on every new intent). */
    private var askedOnLaunch = false

    /** Dimmed streaming screen (IMPROVEMENTS.md 4.1). */
    private var dimmed by mutableStateOf(false)
    private var lastTouch = SystemClock.elapsedRealtime()
    private var swallowGesture = false

    /** Settings > Phone: dim automatically while streaming (persisted). */
    private var autoDim by mutableStateOf(true)

    /** First-run cards (redesign.md 5.7): until the first connection, or Skip for this launch. */
    private var firstRun by mutableStateOf(false)

    private val requestPermissions = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        val acc = pendingAccessory
        pendingAccessory = null
        if (results[Manifest.permission.CAMERA] == true && acc != null) startWebcam(acc)
        if (pendingWireless && granted(Manifest.permission.CAMERA)) setWireless(true)
        pendingWireless = false
    }

    /** Wireless was switched on before camera access was granted. */
    private var pendingWireless = false

    private val usbPermissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != ACTION_USB_PERMISSION) return
            val acc = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java)
            if (acc != null && intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                handleAccessory(acc)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val prefs = getSharedPreferences(WebcamService.PREFS, MODE_PRIVATE)
        WebcamService.showIdleFacing(prefs.getInt(WebcamService.PREF_FACING, Protocol.FACING_BACK))
        WebcamService.showIdlePaused(prefs.getBoolean(WebcamService.PREF_PAUSED, false))
        WebcamService.showIdleCamera(WebcamService.loadCameraSettings(prefs))
        WebcamService.showPairedPcs(PairedPcs(prefs).list())
        autoDim = prefs.getBoolean(WebcamService.PREF_AUTO_DIM, true)
        // First run until the first connection. Phones that already paired a PC have connected before.
        firstRun = !prefs.getBoolean(WebcamService.PREF_FIRST_RUN_DONE, false) && PairedPcs(prefs).list().isEmpty()
        ContextCompat.registerReceiver(
            this, usbPermissionReceiver, IntentFilter(ACTION_USB_PERMISSION), ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        val actions = ScreenActions(
            onFacing = ::setFacing, onPause = ::setPaused, onCommand = ::sendCommand, onDim = { dimmed = true },
            onWireless = ::setWireless, onAnswerPc = ::answerPc, onForgetPc = ::forgetPc, onForgetAllPcs = ::forgetPcs,
            onAutoDim = ::changeAutoDim, onDismissTimedOut = WebcamService::dismissPairingTimedOut,
        )
        setContent {
            MycamTheme {
                val state by WebcamService.state.collectAsState()
                val live = state.streaming && !state.paused
                // Dim once the phone has been left alone for a while during a stream (if the user wants that);
                // any touch wakes it. "Dim screen" in the bottom bar works either way.
                LaunchedEffect(live, autoDim) {
                    if (!live) { dimmed = false; return@LaunchedEffect }
                    if (!autoDim) return@LaunchedEffect
                    while (true) {
                        if (SystemClock.elapsedRealtime() - lastTouch >= DIM_AFTER_MS) dimmed = true
                        delay(1000)
                    }
                }
                LaunchedEffect(dimmed) {
                    window.attributes = window.attributes.apply {
                        screenBrightness = if (dimmed) DIM_BRIGHTNESS else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
                    }
                }
                // The first successful connection ends the first-run cards for good.
                LaunchedEffect(state.connected) {
                    if (state.connected && !prefs.getBoolean(WebcamService.PREF_FIRST_RUN_DONE, false)) {
                        prefs.edit { putBoolean(WebcamService.PREF_FIRST_RUN_DONE, true) }
                        firstRun = false
                    }
                }
                when {
                    dimmed -> DimScreen(state)
                    firstRun && !state.connected && state.pendingPc == null -> FirstRunScreen(onDone = { firstRun = false })
                    else -> WebcamScreen(state = state, actions = actions, autoDim = autoDim)
                }
            }
        }
        handleIntent(intent)
    }

    /** Any touch counts as activity; while dimmed, the whole gesture only wakes the screen. */
    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        lastTouch = SystemClock.elapsedRealtime()
        if (ev.actionMasked == MotionEvent.ACTION_DOWN && dimmed) {
            dimmed = false
            swallowGesture = true
        }
        if (swallowGesture) {
            if (ev.actionMasked == MotionEvent.ACTION_UP || ev.actionMasked == MotionEvent.ACTION_CANCEL) swallowGesture = false
            return true
        }
        return super.dispatchTouchEvent(ev)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    override fun onDestroy() {
        unregisterReceiver(usbPermissionReceiver)
        super.onDestroy()
    }

    private fun handleIntent(intent: Intent?) {
        if (WebcamService.state.value.connected) return
        // Launched by the system when the PC companion switched the phone into accessory mode.
        // The system has already granted access to this accessory.
        val fromIntent = intent?.let {
            IntentCompat.getParcelableExtra(it, UsbManager.EXTRA_ACCESSORY, UsbAccessory::class.java)
        }
        if (fromIntent != null) {
            // Plugged in while locked: come to the foreground so Android lets us start the camera.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1) {
                setShowWhenLocked(true)
                setTurnScreenOn(true)
            }
            handleAccessory(fromIntent)
            return
        }
        // Launched from the home screen while already plugged in: look for the accessory ourselves.
        val usb = getSystemService(UsbManager::class.java)
        val acc = usb.accessoryList?.firstOrNull { it.manufacturer == ACCESSORY_MANUFACTURER }
        if (acc == null) {
            // Opened from the home screen with no PC: ask for camera access now so plugging in later just works.
            val needed = missingPermissions()
            if (needed.isNotEmpty() && !askedOnLaunch) {
                askedOnLaunch = true
                requestPermissions.launch(needed.toTypedArray())
            }
            // Wireless mode was on last time: be findable again while the app is open.
            val prefs = getSharedPreferences(WebcamService.PREFS, MODE_PRIVATE)
            if (prefs.getBoolean(WebcamService.PREF_WIRELESS, false) && !WebcamService.state.value.wirelessOn &&
                granted(Manifest.permission.CAMERA)
            ) {
                setWireless(true)
            }
            return
        }
        if (usb.hasPermission(acc)) {
            handleAccessory(acc)
        } else {
            val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0
            val pi = PendingIntent.getBroadcast(
                this, 0, Intent(ACTION_USB_PERMISSION).setPackage(packageName), flags,
            )
            usb.requestPermission(acc, pi)
        }
    }

    private fun missingPermissions() = buildList {
        if (!granted(Manifest.permission.CAMERA)) add(Manifest.permission.CAMERA)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && !granted(Manifest.permission.POST_NOTIFICATIONS)) {
            add(Manifest.permission.POST_NOTIFICATIONS)
        }
    }

    private fun handleAccessory(acc: UsbAccessory) {
        val needed = missingPermissions()
        if (needed.isEmpty()) {
            startWebcam(acc)
        } else {
            pendingAccessory = acc
            requestPermissions.launch(needed.toTypedArray())
        }
    }

    private fun startWebcam(acc: UsbAccessory) {
        val intent = Intent(this, WebcamService::class.java).putExtra(UsbManager.EXTRA_ACCESSORY, acc)
        ContextCompat.startForegroundService(this, intent)
    }

    private fun setFacing(facing: Int) {
        if (WebcamService.state.value.connected) {
            startService(
                Intent(this, WebcamService::class.java)
                    .setAction(WebcamService.ACTION_SET_FACING)
                    .putExtra(WebcamService.EXTRA_FACING, facing)
            )
        } else {
            getSharedPreferences(WebcamService.PREFS, MODE_PRIVATE).edit()
                .putInt(WebcamService.PREF_FACING, facing).apply()
            WebcamService.showIdleFacing(facing)
        }
    }

    /** Pause or resume. With a PC connected the service applies it (and tells the PC); otherwise it is saved. */
    /** Wireless mode on/off. The service runs it (and keeps running while it is on). */
    private fun setWireless(on: Boolean) {
        if (on && !granted(Manifest.permission.CAMERA)) {
            pendingWireless = true
            requestPermissions.launch(missingPermissions().toTypedArray())
            return
        }
        val intent = Intent(this, WebcamService::class.java)
            .setAction(WebcamService.ACTION_WIRELESS)
            .putExtra(WebcamService.EXTRA_ON, on)
        if (on) ContextCompat.startForegroundService(this, intent)
        else if (WebcamService.state.value.wirelessOn) startService(intent)
        else getSharedPreferences(WebcamService.PREFS, MODE_PRIVATE).edit().putBoolean(WebcamService.PREF_WIRELESS, false).apply()
    }

    private fun forgetPcs() {
        startService(Intent(this, WebcamService::class.java).setAction(WebcamService.ACTION_FORGET_PCS))
    }

    /** Forgets one paired PC (A11). */
    private fun forgetPc(id: String) {
        startService(
            Intent(this, WebcamService::class.java).setAction(WebcamService.ACTION_FORGET_PCS)
                .putExtra(WebcamService.EXTRA_PC_ID, id)
        )
    }

    private fun changeAutoDim(on: Boolean) {
        autoDim = on
        getSharedPreferences(WebcamService.PREFS, MODE_PRIVATE).edit { putBoolean(WebcamService.PREF_AUTO_DIM, on) }
    }

    private fun answerPc(allow: Boolean) {
        startService(
            Intent(this, WebcamService::class.java)
                .setAction(WebcamService.ACTION_WIRELESS_ANSWER)
                .putExtra(WebcamService.EXTRA_ALLOW, allow)
        )
    }

    private fun setPaused(paused: Boolean) {
        if (WebcamService.state.value.connected) {
            startService(
                Intent(this, WebcamService::class.java)
                    .setAction(WebcamService.ACTION_SET_PAUSED)
                    .putExtra(WebcamService.EXTRA_PAUSED, paused)
            )
        } else {
            getSharedPreferences(WebcamService.PREFS, MODE_PRIVATE).edit()
                .putBoolean(WebcamService.PREF_PAUSED, paused).apply()
            WebcamService.showIdlePaused(paused)
        }
    }

    /** Video and camera-control commands from the UI: the service applies them (and tells the PC); with no PC
     * connected, quality / fps / zoom / exposure are saved for next time. */
    private fun sendCommand(cmd: Int, arg: Int) {
        if (WebcamService.state.value.connected) {
            startService(
                Intent(this, WebcamService::class.java).setAction(WebcamService.ACTION_COMMAND)
                    .putExtra(WebcamService.EXTRA_CMD, cmd).putExtra(WebcamService.EXTRA_ARG, arg)
            )
            return
        }
        val prefs = getSharedPreferences(WebcamService.PREFS, MODE_PRIVATE)
        val s = WebcamService.loadCameraSettings(prefs)
        val updated = when (cmd) {
            Protocol.CMD_SET_QUALITY -> s.copy(quality = arg)
            Protocol.CMD_SET_FPS -> s.copy(fps = arg)
            Protocol.CMD_SET_ZOOM -> s.copy(zoom = arg / 10f)
            Protocol.CMD_SET_EXPOSURE -> s.copy(ev = arg.toByte().toInt())
            else -> s
        }
        WebcamService.saveCameraSettings(prefs, updated)
        WebcamService.showIdleCamera(updated)
    }

    private fun granted(permission: String) =
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

    companion object {
        private const val ACTION_USB_PERMISSION = "io.github.whoissaaif.mycam.USB_PERMISSION"
        const val ACCESSORY_MANUFACTURER = "MyCam"
        private const val DIM_AFTER_MS = 30_000L
        private const val DIM_BRIGHTNESS = 0.02f
    }
}
