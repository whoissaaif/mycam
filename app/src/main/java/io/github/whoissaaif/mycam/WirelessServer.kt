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
 * Every discovery probe goes to [onProbe] (IP address, the port it came from, PC name; on the discovery
 * thread) for "Scan for PCs". That port is where [askToConnect] sends its invitation.
 */
class WirelessServer(
    private val context: Context,
    private val onProbe: (ip: String, port: Int, name: String) -> Unit,
    private val onClient: (Socket, String) -> Unit,
    /** The PC at that address turned down a QR invite: "code" or "busy" (PROTOCOL.md "QR pairing"). */
    private val onRefusal: (ip: String, reason: String) -> Unit = { _, _ -> },
) {
    @Volatile private var running = false
    @Volatile private var server: ServerSocket? = null
    @Volatile private var discovery: DatagramSocket? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private val threads = mutableListOf<Thread>()

    /** PC names from their discovery messages, by IP address (shown when a PC asks to connect). */
    private val pcNames = ConcurrentHashMap<String, String>()

    /**
     * Main thread. Binds both sockets here, before any thread starts, so a [stop] straight after always has
     * them to close. When the threads bound them, a quick stop found nothing to close, the threads bound
     * afterwards and ran for good, and the next start failed with "address in use". Binding does no network
     * I/O, so the main thread is fine. Returns false (and logs why) if a port can't be bound; nothing is left
     * open then.
     */
    @Synchronized
    fun start(): Boolean {
        if (running) return true
        val tcp = try {
            ServerSocket().apply { reuseAddress = true; bind(InetSocketAddress(Protocol.WIRELESS_TCP_PORT)) }
        } catch (e: IOException) {
            Log.w(TAG, "can't listen on port ${Protocol.WIRELESS_TCP_PORT}: ${e.message}")
            return false
        }
        val udp = try {
            DatagramSocket(null).apply {
                reuseAddress = true
                broadcast = true
                bind(InetSocketAddress(Protocol.WIRELESS_DISCOVERY_PORT))
            }
        } catch (e: IOException) {
            Log.w(TAG, "can't listen on port ${Protocol.WIRELESS_DISCOVERY_PORT}: ${e.message}")
            try { tcp.close() } catch (_: IOException) {}
            return false
        }
        server = tcp
        discovery = udp
        running = true
        // Some Wi-Fi drivers drop broadcast packets unless an app holds this lock.
        multicastLock = context.applicationContext.getSystemService(WifiManager::class.java)
            ?.createMulticastLock("MyCam:discovery")?.apply { setReferenceCounted(false); acquire() }
        threads += Thread({ acceptLoop(tcp) }, "wireless-accept").apply { start() }
        threads += Thread({ discoveryLoop(udp) }, "wireless-discovery").apply { start() }
        return true
    }

    /** Main thread. Closes both sockets, which ends their threads. */
    @Synchronized
    fun stop() {
        running = false
        try { server?.close() } catch (_: IOException) {}
        discovery?.close()
        server = null
        discovery = null
        multicastLock?.release()
        multicastLock = null
        threads.forEach { it.interrupt() }
        // On Linux (Android included) closing a socket that a thread is blocked in frees the port only once
        // that thread has returned: wait for both, briefly, so a start() right after this can bind again.
        threads.forEach { if (it !== Thread.currentThread()) it.join(250) }
        threads.clear()
    }

    private fun acceptLoop(s: ServerSocket) {
        try {
            while (running) {
                val client = s.accept()
                if (!running) { try { client.close() } catch (_: IOException) {}; break }
                val ip = client.inetAddress.hostAddress ?: ""
                onClient(client, pcNames[ip] ?: ip)
            }
        } catch (e: IOException) {
            if (running) Log.w(TAG, "accept ended: ${e.message}")
        } finally {
            try { s.close() } catch (_: IOException) {}
        }
    }

    /** Answers "MYCAM?1 <pc name>" broadcasts with "MYCAM!1 <tcp port> <phone name>". */
    private fun discoveryLoop(socket: DatagramSocket) {
        try {
            val buf = ByteArray(512)
            val reply = "${Protocol.WIRELESS_HERE} ${Protocol.WIRELESS_TCP_PORT} ${deviceName(context)}".toByteArray()
            while (running) {
                val packet = DatagramPacket(buf, buf.size)
                socket.receive(packet)
                val text = String(packet.data, 0, packet.length, Charsets.UTF_8)
                if (text.startsWith(Protocol.WIRELESS_REFUSE + " ")) {
                    packet.address.hostAddress?.let { onRefusal(it, text.substringAfter(' ').trim().take(16)) }
                    continue
                }
                if (!text.startsWith(Protocol.WIRELESS_ASK)) continue
                val ip = packet.address.hostAddress ?: continue
                // The PC's name, cleaned as the PC cleans a phone's (it reaches the pairing question).
                val space = (0 until packet.length).firstOrNull { packet.data[it] == ' '.code.toByte() }
                val name = space?.let { PcNames.clean(packet.data.copyOfRange(it + 1, packet.length), "") }
                    ?.takeIf { it.isNotEmpty() }
                name?.let { pcNames[ip] = it }
                onProbe(ip, packet.port, name ?: ip)
                socket.send(DatagramPacket(reply, reply.size, packet.address, packet.port))
            }
        } catch (e: IOException) {
            if (running) Log.w(TAG, "discovery ended: ${e.message}")
        } finally {
            socket.close()
        }
    }

    /**
     * Asks the PC at [ip]:[udpPort] to connect to this phone (PROTOCOL.md "The phone asks for a session"),
     * so the user can start a pairing from the phone instead of the PC. [udpPort] is the port that PC's
     * probe came from, so this only reaches a PC that is looking for phones right now.
     *
     * Called from the main thread (the user tapped a row), so the datagram goes out on a thread of its
     * own: a send on the main looper is a NetworkOnMainThreadException, not a slow send. Fire and forget —
     * the PC answers by opening a TCP session, and the caller resends a few times if it doesn't.
     */
    fun askToConnect(ip: String, udpPort: Int) =
        send(ip, udpPort, "${Protocol.WIRELESS_CONNECT} ${Protocol.WIRELESS_TCP_PORT} ${deviceName(context)}")

    /**
     * QR pairing (PROTOCOL.md "QR pairing"): asks the PC whose QR code was scanned to connect, with proof that
     * this phone holds its one-time [code]. Sent to the port its probe came from, like [askToConnect].
     */
    fun qrInvite(ip: String, udpPort: Int, code: ByteArray) =
        send(ip, udpPort, QrPairing.invite(code, Protocol.WIRELESS_TCP_PORT, deviceName(context)))

    /** One datagram from the discovery socket, on a thread of its own (callers are on the main thread). */
    private fun send(ip: String, udpPort: Int, text: String) {
        val socket = discovery ?: return
        if (udpPort <= 0 || udpPort > 65535) return
        val message = text.toByteArray(Charsets.UTF_8)
        Thread({
            try {
                socket.send(DatagramPacket(message, message.size, InetAddress.getByName(ip), udpPort))
            } catch (e: IOException) {
                Log.w(TAG, "can't send to $ip:$udpPort: ${e.message}")
            } catch (e: SecurityException) {
                Log.w(TAG, "can't send to $ip:$udpPort: ${e.message}")
            }
        }, "wireless-send").start()
    }


    companion object {
        private const val TAG = "WirelessServer"

        /** The phone's name as the user set it (Settings > About), else maker and model. */
        fun deviceName(context: Context): String {
            val userName = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1)
                Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME) else null
            return (userName?.takeIf { it.isNotBlank() } ?: "${Build.MANUFACTURER} ${Build.MODEL}").take(64)
        }

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
