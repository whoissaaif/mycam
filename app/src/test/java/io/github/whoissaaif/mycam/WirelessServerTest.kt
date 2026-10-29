package io.github.whoissaaif.mycam

import org.junit.After
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.net.ConnectException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Turning Wi-Fi mode off and on quickly must leave nothing bound behind. */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class WirelessServerTest {
    private val app = RuntimeEnvironment.getApplication()
    private var clients = CountDownLatch(1)
    private val server = WirelessServer(app, onProbe = { _, _, _ -> }, onClient = { sock, _ -> sock.close(); clients.countDown() })

    @After fun tearDown() = server.stop()

    private fun connect(): Socket =
        Socket().apply { connect(InetSocketAddress(InetAddress.getLoopbackAddress(), Protocol.WIRELESS_TCP_PORT), 2_000) }

    @Test fun quickOffAndOnStillAcceptsPcs() {
        repeat(10) {
            assertTrue(server.start())
            server.stop()
        }
        assertTrue(server.start())
        connect().close()
        assertTrue(clients.await(5, TimeUnit.SECONDS))
    }

    @Test fun stopFreesBothPorts() {
        assertTrue(server.start())
        server.stop()
        // A port left bound by a thread that outlived stop() would make these fail.
        ServerSocket().apply { reuseAddress = true; bind(InetSocketAddress(Protocol.WIRELESS_TCP_PORT)) }.close()
        java.net.DatagramSocket(null).apply {
            reuseAddress = true
            bind(InetSocketAddress(Protocol.WIRELESS_DISCOVERY_PORT))
        }.close()
        var refused = false
        try { connect().close() } catch (_: ConnectException) { refused = true }
        assertTrue(refused)
    }

    @Test fun portInUseFailsWithoutThrowing() {
        ServerSocket().use { other ->
            other.bind(InetSocketAddress(Protocol.WIRELESS_TCP_PORT))
            assertFalse(server.start())
        }
        // Nothing was left half open: once the port is free, it starts.
        assertTrue(server.start())
    }
}
