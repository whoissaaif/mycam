package io.github.whoissaaif.mycam

import android.content.Context
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.security.MessageDigest

/**
 * A full code pairing against a fake PC. Pairing again under an id the phone already has a key for says so
 * in the question, and doesn't carry the camera permission over to the new key.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class WifiRePairTest {
    private val pcs = PairedPcs(RuntimeEnvironment.getApplication().getSharedPreferences("repair", Context.MODE_PRIVATE))
    private val pcId = ByteArray(16) { 7 }

    private class Asked(val name: String, val rePair: Boolean)

    /** Runs a code pairing (the user answers [allow]); returns what was asked and the key the PC derived. */
    private fun pair(flags: Int, pcName: ByteArray, allow: Boolean = true): Pair<Asked?, ByteArray?> {
        var asked: Asked? = null
        ServerSocket(0, 1, InetAddress.getLoopbackAddress()).use { server ->
            Socket(InetAddress.getLoopbackAddress(), server.localPort).use { pc ->
                val phoneSide = server.accept()
                val phone = Thread {
                    WifiHandshake(
                        phoneSide, pcs, pcs.phoneId(), "Test phone",
                        askUser = { name, _, rePair, _ -> asked = Asked(name, rePair); allow },
                        pairingWanted = { true },
                    ).run()
                }.apply { start() }

                val transcript = MessageDigest.getInstance("SHA-256")
                val out = DataOutputStream(pc.getOutputStream())
                val input = DataInputStream(pc.getInputStream())
                fun send(type: Int, body: ByteArray, record: Boolean = true) {
                    val frame = ByteArrayOutputStream().apply {
                        DataOutputStream(this).apply { writeShort(body.size + 1); writeByte(type); write(body) }
                    }.toByteArray()
                    if (record) transcript.update(frame)
                    out.write(frame); out.flush()
                }
                fun read(record: Boolean = true): Pair<Int, ByteArray> {
                    val len = input.readUnsignedShort()
                    val type = input.readUnsignedByte()
                    val body = ByteArray(len - 1).also { input.readFully(it) }
                    if (record) transcript.update(byteArrayOf((len shr 8).toByte(), len.toByte(), type.toByte()) + body)
                    return type to body
                }

                val key = WifiCrypto.newKeyPair()
                val pcPub = WifiCrypto.rawPublic(key.public)
                val npc = ByteArray(16) { 5 }
                send(WifiHandshake.TYPE_CLIENT_HELLO, ByteArrayOutputStream().apply {
                    write("MCHS".toByteArray()); write(1); write(flags)
                    write(pcId); write(pcPub); write(npc)
                    write(pcName.size); write(pcName)
                }.toByteArray())
                val (helloType, hello) = read()
                assertEquals(WifiHandshake.TYPE_SERVER_HELLO, helloType)
                val phonePub = hello.copyOfRange(16, 81)
                val nph = hello.copyOfRange(81, 97)
                assertEquals(1, hello[97].toInt()) // Pairing mode.
                val na = ByteArray(16) { 9 }
                send(WifiHandshake.TYPE_NONCE_A, na)
                val (_, nb) = read()
                val z = WifiCrypto.ecdh(key.private, WifiCrypto.publicFromRaw(phonePub))
                val pairKey = WifiCrypto.pairKey(z, na, nb)
                val keys = WifiCrypto.sessionKeys(z, pairKey, npc, nph)
                val h = (transcript.clone() as MessageDigest).digest()
                send(WifiHandshake.TYPE_PC_FINISHED, WifiCrypto.finished(keys.copyOfRange(64, 96), "PC", h), record = false)
                val (finType, _) = read(record = false)
                pc.close()
                phone.join(5_000)
                return asked to (if (finType == WifiHandshake.TYPE_PHONE_FINISHED) pairKey else null)
            }
        }
    }

    @Test fun newPcIsAllowedTheCameraByTheCodePairing() {
        val (asked, key) = pair(flags = 0, pcName = "DESKTOP-NEW".toByteArray())
        assertNotNull(key)
        assertFalse(asked!!.rePair)
        assertArrayEquals(key, pcs.key(pcId))
        assertTrue(pcs.cameraAllowed(pcId))
    }

    @Test fun pairingAgainSaysSoAndAsksForTheCameraAgain() {
        pcs.put(pcId, ByteArray(32) { 1 }, "DESKTOP-X")
        pcs.allowCamera(pcId)
        val (asked, key) = pair(flags = 0x01, pcName = "DESKTOP-X".toByteArray())
        assertNotNull(key)
        assertTrue(asked!!.rePair)
        assertArrayEquals(key, pcs.key(pcId)) // Allowed: the new key replaces the old one...
        assertFalse(pcs.cameraAllowed(pcId))  // ...but the camera has to be allowed again.
    }

    @Test fun refusingToPairAgainKeepsTheOldKey() {
        val old = ByteArray(32) { 1 }
        pcs.put(pcId, old, "DESKTOP-X")
        pcs.allowCamera(pcId)
        val (asked, _) = pair(flags = 0x01, pcName = "DESKTOP-X".toByteArray(), allow = false)
        assertTrue(asked!!.rePair)
        assertArrayEquals(old, pcs.key(pcId))
        assertTrue(pcs.cameraAllowed(pcId))
    }

    @Test fun thePcNameIsCleanedBeforeItIsShown() {
        val (asked, _) = pair(flags = 0, pcName = "DESKTOP‮-X\n Allow".toByteArray())
        assertEquals("DESKTOP-XAllow", asked!!.name)
    }
}
