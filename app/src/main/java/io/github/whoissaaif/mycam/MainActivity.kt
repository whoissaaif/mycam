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
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import io.github.whoissaaif.mycam.ui.WebcamScreen
import io.github.whoissaaif.mycam.ui.theme.MycamTheme

class MainActivity : ComponentActivity() {

    /** Accessory waiting for the camera permission before the service can start. */
    private var pendingAccessory: UsbAccessory? = null

    /** Whether we already asked for permissions this launch (avoid re-asking on every new intent). */
    private var askedOnLaunch = false

    private val requestPermissions = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { results ->
        val acc = pendingAccessory
        pendingAccessory = null
        if (results[Manifest.permission.CAMERA] == true && acc != null) startWebcam(acc)
    }

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
        ContextCompat.registerReceiver(
            this, usbPermissionReceiver, IntentFilter(ACTION_USB_PERMISSION), ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        setContent {
            MycamTheme {
                val state by WebcamService.state.collectAsState()
                WebcamScreen(state = state, onFacing = ::setFacing, onPause = ::setPaused, onCommand = ::sendCommand)
            }
        }
        handleIntent(intent)
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
    }
}
