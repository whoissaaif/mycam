package com.example.mycam

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
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import com.example.mycam.ui.theme.MycamTheme

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
        WebcamService.showIdleFacing(
            getSharedPreferences(WebcamService.PREFS, MODE_PRIVATE).getInt(WebcamService.PREF_FACING, Protocol.FACING_BACK)
        )
        ContextCompat.registerReceiver(
            this, usbPermissionReceiver, IntentFilter(ACTION_USB_PERMISSION), ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        setContent {
            MycamTheme {
                val state by WebcamService.state.collectAsState()
                Scaffold(modifier = Modifier.fillMaxSize()) { innerPadding ->
                    WebcamScreen(
                        state = state,
                        onFacing = ::setFacing,
                        modifier = Modifier.padding(innerPadding),
                    )
                }
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

    private fun granted(permission: String) =
        ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

    companion object {
        private const val ACTION_USB_PERMISSION = "com.example.mycam.USB_PERMISSION"
        const val ACCESSORY_MANUFACTURER = "MyCam"
    }
}

@Composable
fun WebcamScreen(state: WebcamService.UiState, onFacing: (Int) -> Unit, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier.fillMaxSize().padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        val (title, detail) = when {
            state.error != null -> stringResource(R.string.status_error) to state.error
            state.streaming -> stringResource(R.string.status_streaming) to state.resolution
            state.connected -> stringResource(R.string.status_idle) to stringResource(R.string.status_idle_detail)
            else -> stringResource(R.string.status_disconnected) to stringResource(R.string.status_disconnected_detail)
        }
        Text(title, style = MaterialTheme.typography.headlineMedium, textAlign = TextAlign.Center)
        Spacer(Modifier.height(8.dp))
        Text(detail, style = MaterialTheme.typography.bodyLarge, textAlign = TextAlign.Center)
        Spacer(Modifier.height(32.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.fillMaxWidth()) {
            FacingButton(stringResource(R.string.camera_back), state.facing == Protocol.FACING_BACK, Modifier.weight(1f)) {
                onFacing(Protocol.FACING_BACK)
            }
            FacingButton(stringResource(R.string.camera_front), state.facing == Protocol.FACING_FRONT, Modifier.weight(1f)) {
                onFacing(Protocol.FACING_FRONT)
            }
        }
    }
}

@Composable
private fun FacingButton(label: String, selected: Boolean, modifier: Modifier, onClick: () -> Unit) {
    if (selected) Button(onClick = onClick, modifier = modifier) { Text(label) }
    else OutlinedButton(onClick = onClick, modifier = modifier) { Text(label) }
}

@Preview(showBackground = true)
@Composable
fun WebcamScreenPreview() {
    MycamTheme {
        WebcamScreen(WebcamService.UiState(connected = true, streaming = true, resolution = "1920×1080"), {})
    }
}
