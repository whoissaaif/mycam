package io.github.whoissaaif.mycam

import android.content.Context
import android.net.ConnectivityManager
import android.net.wifi.WifiManager
import android.os.Build
import android.provider.Settings
import android.util.Log
import java.io.IOException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.ConcurrentHashMap

/**
 * Wireless mode (IMPROVEMENTS.md 11, phase 1): makes this phone findable by MyCam PCs on the same network
 * and accepts their connections. The PC is always the one that connects, so Windows never needs a firewall
 * rule. Discovery and the TCP stream are described in protocol/PROTOCOL.md ("Wireless transport").
 *
 * Every accepted connection goes to [onClient]; the service asks the user before anything is streamed.
 */
class WirelessServer(private val context: Context, private val onClient: (Socket, String) -> Unit) {
    @Volatile private var running = false
    private var server: ServerSocket? = null
    private var discovery: DatagramSocket? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private val threads = mutableListOf<Thread>()

    /** PC names from their discovery messages, by IP address (shown when a PC asks to connect). */
    private val pcNames = ConcurrentHashMap<String, String>()

    fun start() {
        if (running) return
        running = true
        // Some Wi-Fi drivers drop broadcast packets unless an app holds this lock.
        multicastLock = context.applicationContext.getSystemService(WifiManager::class.java)
            ?.createMulticastLock("MyCam:discovery")?.apply { setReferenceCounted(false); acquire() }
        threads += Thread(::acceptLoop, "wireless-accept").apply { start() }
        threads += Thread(::discoveryLoop, "wireless-discovery").apply { start() }
    }

    fun stop() {
        running = false
        try { server?.close() } catch (_: IOException) {}
        discovery?.close()
        server = null
        discovery = null
        multicastLock?.release()
        multicastLock = null
        threads.forEach { it.interrupt() }
        threads.clear()
    }

    private fun acceptLoop() {
        try {
            val s = ServerSocket().apply { reuseAddress = true; bind(InetSocketAddress(Protocol.WIRELESS_TCP_PORT)) }
            server = s
            while (running) {
                val client = s.accept()
                val ip = client.inetAddress.hostAddress ?: ""
                onClient(client, pcNames[ip] ?: ip)
            }
        } catch (e: IOException) {
            if (running) Log.w(TAG, "accept ended: ${e.message}")
        }
    }

    /** Answers "MYCAM?1 <pc name>" broadcasts with "MYCAM!1 <tcp port> <phone name>". */
    private fun discoveryLoop() {
        try {
            val socket = DatagramSocket(null).apply {
                reuseAddress = true
                broadcast = true
                bind(InetSocketAddress(Protocol.WIRELESS_DISCOVERY_PORT))
            }
            discovery = socket
            val buf = ByteArray(512)
            val reply = "${Protocol.WIRELESS_HERE} ${Protocol.WIRELESS_TCP_PORT} ${deviceName()}".toByteArray()
            while (running) {
                val packet = DatagramPacket(buf, buf.size)
                socket.receive(packet)
                val text = String(packet.data, 0, packet.length, Charsets.UTF_8)
                if (!text.startsWith(Protocol.WIRELESS_ASK)) continue
                val ip = packet.address.hostAddress ?: continue
                text.substringAfter(' ', "").trim().take(64).takeIf { it.isNotEmpty() }?.let { pcNames[ip] = it }
                socket.send(DatagramPacket(reply, reply.size, packet.address, packet.port))
            }
        } catch (e: IOException) {
            if (running) Log.w(TAG, "discovery ended: ${e.message}")
        }
    }

    private fun deviceName(): String {
        val userName = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1)
            Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME) else null
        return (userName?.takeIf { it.isNotBlank() } ?: "${Build.MANUFACTURER} ${Build.MODEL}").take(64)
    }

    companion object {
        private const val TAG = "WirelessServer"

        /**
         * This phone's IPv4 address on Wi-Fi (or its own hotspot), for showing in the app; null if it has none.
         * Wi-Fi and hotspot interfaces come first because the active network may be mobile data.
         */
        fun localAddress(context: Context): String? {
            val wifiLike = { name: String -> name.startsWith("wlan") || name.startsWith("swlan") || name.startsWith("ap") }
            val candidates = try {
                NetworkInterface.getNetworkInterfaces()?.toList().orEmpty()
                    .filter { it.isUp && !it.isLoopback }
                    .sortedByDescending { wifiLike(it.name) }
                    .flatMap { nic -> nic.inetAddresses.toList().filterIsInstance<Inet4Address>().map { nic.name to it } }
            } catch (_: Exception) { emptyList() }
            candidates.firstOrNull { wifiLike(it.first) }?.let { return it.second.hostAddress }
            val cm = context.getSystemService(ConnectivityManager::class.java) ?: return null
            val props = cm.getLinkProperties(cm.activeNetwork ?: return null) ?: return null
            return props.linkAddresses.map { it.address }.firstOrNull { it is Inet4Address && !it.isLoopbackAddress }
                ?.let { (it as InetAddress).hostAddress }
        }
    }
}
