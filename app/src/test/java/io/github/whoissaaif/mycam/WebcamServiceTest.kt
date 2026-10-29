package io.github.whoissaaif.mycam

import android.app.Notification
import android.app.NotificationManager
import android.os.PowerManager
import org.robolectric.annotation.Implementation
import org.robolectric.annotation.Implements
import java.util.concurrent.Executor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowPowerManager
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket

/** Robolectric's PowerManager has no thermal service behind it; the service only needs the call to succeed. */
@Implements(PowerManager::class)
class NoThermalPowerManager : ShadowPowerManager() {
    @Implementation
    protected fun addThermalStatusListener(executor: Executor, listener: PowerManager.OnThermalStatusChangedListener) {}

    @Implementation
    protected fun removeThermalStatusListener(listener: PowerManager.OnThermalStatusChangedListener) {}
}

/** How a finished Wi-Fi handshake becomes the link, or doesn't: never after onDestroy, and a PC not allowed the camera yet gets a notification. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], shadows = [NoThermalPowerManager::class])
class WebcamServiceTest {
    private val app = RuntimeEnvironment.getApplication()
    private val notifications = app.getSystemService(NotificationManager::class.java)

    private fun WebcamService.setWirelessOn() =
        WebcamService::class.java.getDeclaredField("wirelessOn").apply { isAccessible = true }.setBoolean(this, true)

    private fun WebcamService.finishHandshake(sock: Socket, result: WifiHandshake.Result) =
        WebcamService::class.java.getDeclaredMethod("finishHandshake", Socket::class.java, WifiHandshake.Result::class.java)
            .apply { isAccessible = true }.invoke(this, sock, result)

    /** A connected loopback pair: the phone's end goes to [block], the PC's end stays open meanwhile. */
    private fun withPc(block: (phoneSide: Socket) -> Unit) {
        ServerSocket(0, 1, InetAddress.getLoopbackAddress()).use { server ->
            Socket(InetAddress.getLoopbackAddress(), server.localPort).use { _ -> block(server.accept()) }
        }
    }

    private fun resultFor(sock: Socket) =
        WifiHandshake.Result(sock.getInputStream(), sock.getOutputStream(), "DESKTOP-TEST", false, ByteArray(16) { 4 })

    private fun gateNotification(): Notification? =
        shadowOf(notifications).allNotifications.firstOrNull { shadowOf(it).contentTitle?.contains("DESKTOP-TEST") == true }

    @Test fun handshakeFinishingAfterDestroyIsDropped() {
        val controller = Robolectric.buildService(WebcamService::class.java).create()
        val service = controller.get()
        service.setWirelessOn()
        controller.destroy()
        withPc { phoneSide ->
            service.finishHandshake(phoneSide, resultFor(phoneSide))
            assertTrue(phoneSide.isClosed)
            assertNull(ShadowPowerManager.getLatestWakeLock()) // No link, so no wake lock was ever taken.
            assertNull(gateNotification())
        }
    }

    @Test fun gatedPcGetsANotificationThatNeedsUnlockingToAllow() {
        val controller = Robolectric.buildService(WebcamService::class.java).create()
        val service = controller.get()
        service.setWirelessOn()
        withPc { phoneSide ->
            service.finishHandshake(phoneSide, resultFor(phoneSide))
            assertTrue(!phoneSide.isClosed)
            assertNotNull(ShadowPowerManager.getLatestWakeLock())

            val n = assertNotNull(gateNotification())
            assertEquals(Notification.VISIBILITY_PRIVATE, n.visibility)
            assertTrue(n.publicVersion.actions.isNullOrEmpty()) // Locked: nothing to tap.
            val allow = n.actions.single { it.title.toString() == app.getString(R.string.wireless_allow) }
            assertTrue(allow.isAuthenticationRequired)

            controller.destroy() // The link ends: the question goes with it.
            assertNull(gateNotification())
            assertTrue(phoneSide.isClosed)
        }
    }

    private fun <T> assertNotNull(value: T?): T { org.junit.Assert.assertNotNull(value); return value!! }
}
