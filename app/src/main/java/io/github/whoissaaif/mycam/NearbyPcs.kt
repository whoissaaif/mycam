package io.github.whoissaaif.mycam

/** A MyCam PC whose discovery probe ("MYCAM?1 <pc name>") reached this phone recently. */
data class NearbyPc(val name: String, val ip: String)

/** How a nearby PC relates to this phone, in the order the list shows them. */
enum class NearbyStatus { Connected, Paired, NotPaired }

/**
 * The PCs that are looking for phones on this Wi-Fi ("Scan for PCs"). The phone never probes: a PC with
 * "Find phones on Wi-Fi" on broadcasts every 2 s, so a PC that has been quiet for [maxAgeMs] (three probes)
 * has gone. Keyed by IP address; the time comes from the caller, so tests need no clock.
 * Thread-safe (the discovery thread may feed it while the main thread reads it).
 */
class NearbyPcs(private val maxAgeMs: Long = MAX_AGE_MS) {
    private class Entry(var name: String, var lastSeen: Long)

    private val byIp = HashMap<String, Entry>()

    /** A probe from [ip] arrived at [now]. Returns true if the list changed (a new PC, or a new name). */
    @Synchronized
    fun seen(ip: String, name: String, now: Long): Boolean {
        val shown = name.trim().ifEmpty { ip }
        val e = byIp[ip]
        if (e == null || now - e.lastSeen > maxAgeMs) {
            byIp[ip] = Entry(shown, now)
            return true
        }
        e.lastSeen = now
        if (e.name == shown) return false
        e.name = shown
        return true
    }

    /** The PCs seen in the last [maxAgeMs] at [now], by name (then IP); older ones are dropped. */
    @Synchronized
    fun list(now: Long): List<NearbyPc> {
        byIp.entries.removeAll { now - it.value.lastSeen > maxAgeMs }
        return byIp.map { (ip, e) -> NearbyPc(e.name, ip) }
            .sortedWith(compareBy<NearbyPc> { it.name.lowercase() }.thenBy { ipSortKey(it.ip) })
    }

    @Synchronized
    fun isEmpty(): Boolean = byIp.isEmpty()

    @Synchronized
    fun clear() = byIp.clear()

    companion object {
        /** Three of the PC's 2 s probe intervals. Also how long a scan shows its progress bar. */
        const val MAX_AGE_MS = 6_000L

        /** "192.168.1.9" sorts before "192.168.1.10". */
        private fun ipSortKey(ip: String): String =
            ip.split('.').joinToString(".") { part -> if (part.all(Char::isDigit)) part.padStart(3, '0') else part }

        /**
         * Best-effort status of [pc]. Discovery only carries the PC's name, so pairing is matched by name
         * (Windows computer names ignore case). The connected PC is matched by IP when it is known.
         */
        fun status(pc: NearbyPc, paired: List<PairedPc>, connectedIp: String?, connectedName: String?): NearbyStatus = when {
            connectedIp != null && pc.ip == connectedIp -> NearbyStatus.Connected
            connectedIp == null && connectedName != null && pc.name.equals(connectedName, ignoreCase = true) -> NearbyStatus.Connected
            paired.any { it.name.trim().equals(pc.name, ignoreCase = true) } -> NearbyStatus.Paired
            else -> NearbyStatus.NotPaired
        }

        /** The list as shown: the connected PC first, then paired ones, then new ones (each by name). */
        fun ordered(pcs: List<NearbyPc>, paired: List<PairedPc>, connectedIp: String?, connectedName: String?) =
            pcs.map { it to status(it, paired, connectedIp, connectedName) }.sortedBy { it.second.ordinal }
    }
}
