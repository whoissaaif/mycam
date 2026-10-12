package io.github.whoissaaif.mycam

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * Checks QR pairing (PROTOCOL.md "QR pairing") against protocol/golden.txt "qr.*" (made with the JDK); the PC's
 * tests check the same vectors. qr.fingerprint and qr.key_text are text, the rest hex.
 */
class QrPairingTest {
    private val golden: Map<String, String> by lazy {
        File("../protocol/golden.txt").readLines()
            .map { it.substringBefore('#') }
            .filter { '=' in it }
            .associate { it.substringBefore('=').trim() to it.substringAfter('=').trim() }
    }

    private fun g(name: String) = hex(golden.getValue(name))
    private fun hex(s: String) = s.replace(" ", "").chunked(2).map { it.toInt(16).toByte() }.toByteArray()

    @Test
    fun fingerprintAndProofMatchGolden() {
        assertEquals(golden.getValue("qr.fingerprint"), QrPairing.fingerprint(g("qr.cert_der")))
        val port = g("qr.port").let { (it[0].toInt() and 0xFF shl 8) or (it[1].toInt() and 0xFF) }
        assertEquals(47800, port)
        assertArrayEquals(g("qr.connect_proof"), QrPairing.connectProof(g("qr.key"), g("qr.phone_id"), port))
        assertEquals(golden.getValue("qr.key_text"), QrPairing.encodeBase64Url(g("qr.key")))
        assertArrayEquals(g("qr.key"), QrPairing.decodeBase64Url(golden.getValue("qr.key_text")))
    }

    @Test
    fun base64UrlRoundTripsAndRefusesOtherSpellings() {
        for (n in 0 until 40) {
            val b = ByteArray(n) { (it * 37 + 11).toByte() }
            assertArrayEquals(b, QrPairing.decodeBase64Url(QrPairing.encodeBase64Url(b)))
        }
        assertArrayEquals(byteArrayOf(0xFF.toByte(), 0xEF.toByte()), QrPairing.decodeBase64Url("_-8"))
        assertNull(QrPairing.decodeBase64Url("QUJD="))
        assertNull(QrPairing.decodeBase64Url("QUJD+"))
        assertNull(QrPairing.decodeBase64Url("Q"))
        assertNull(QrPairing.decodeBase64Url("QR")) // Leftover bits set.
    }

    @Test
    fun readsTheSameQrCodeIphonesScan() {
        val f = golden.getValue("qr.fingerprint")
        val ok = QrPairing.parse("https://192.168.1.20:47900/?t=AbC-_x12&f=$f")
        assertTrue(ok is QrPairing.Scan.Ok)
        val code = (ok as QrPairing.Scan.Ok).code
        assertEquals("192.168.1.20", code.host)
        assertEquals("AbC-_x12", code.code)
        assertArrayEquals(QrPairing.decodeBase64Url(f), code.fingerprint)
        // Field order doesn't matter, and no path is the same as "/".
        assertTrue(QrPairing.parse("https://10.0.0.5:47900?f=$f&t=x") is QrPairing.Scan.Ok)

        // A companion before 1.5: no fingerprint, so MyCam on the PC needs updating.
        assertEquals(QrPairing.Scan.OldPc, QrPairing.parse("https://192.168.1.20:47900/?t=AbC"))
        assertEquals(QrPairing.Scan.OldPc, QrPairing.parse("https://192.168.1.20:47900/?t=AbC&f=short"))
        // Anything else isn't ours.
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("https://example.com/?t=a&f=$f"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("http://192.168.1.20:47900/?t=a&f=$f"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("https://192.168.1.20:47900/other?t=a&f=$f"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("https://192.168.1.20:47900/?f=$f"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("WIFI:S:home;T:WPA;P:secret;;"))
        assertEquals(QrPairing.Scan.NotMyCam, QrPairing.parse("not a url at all %%%"))
    }

    @Test
    fun hexBothWays() {
        val id = g("qr.phone_id")
        assertEquals("303132333435363738393a3b3c3d3e3f", QrPairing.hex(id))
        assertArrayEquals(id, QrPairing.unhex("303132333435363738393a3b3c3d3e3f"))
        assertNull(QrPairing.unhex("303"))
        assertNull(QrPairing.unhex("zz"))
    }
}
