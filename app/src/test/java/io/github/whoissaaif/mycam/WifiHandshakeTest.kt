package io.github.whoissaaif.mycam

import android.content.Context
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
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
import java.util.concurrent.atomic.AtomicBoolean

/** A PC this phone has no key for only gets a pairing question after the user asked to pair (#69). */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class WifiHandshakeTest {
    private val pcs = PairedPcs(RuntimeEnvironment.getApplication().getSharedPreferences("test", Context.MODE_PRIVATE))

    /** Runs the phone's side against a fake PC that sends CLIENT_HELLO; returns the first reply's type and body. */
    private fun firstReply(pairingWanted: Boolean, flags: Int = 0, asked: AtomicBoolean): Pair<Int, ByteArray> {
        ServerSocket(0, 1, InetAddress.getLoopbackAddress()).use { server ->
            Socket(InetAddress.getLoopbackAddress(), server.localPort).use { pc ->
                val phoneSide = server.accept()
                val phone = Thread {
                    WifiHandshake(
                        phoneSide, pcs, pcs.phoneId(), "Test phone",
                        askUser = { _, _, _ -> asked.set(true); false },
                        pairingWanted = { pairingWanted },
                    ).run()
                }.apply { start() }

                val name = "DESKTOP-NEW".toByteArray()
                val body = ByteArrayOutputStream().apply {
                    write("MCHS".toByteArray()); write(1); write(flags)
                    write(ByteArray(16) { 3 })                                    // PC id
                    write(WifiCrypto.rawPublic(WifiCrypto.newKeyPair().public))  // ePK
                    write(ByteArray(16) { 5 })                                    // Npc
                    write(name.size); write(name)
                }.toByteArray()
                DataOutputStream(pc.getOutputStream()).apply {
                    writeShort(body.size + 1); writeByte(WifiHandshake.TYPE_CLIENT_HELLO); write(body); flush()
                }

                val input = DataInputStream(pc.getInputStream())
                val len = input.readUnsignedShort()
                val type = input.readUnsignedByte()
                val reply = ByteArray(len - 1).also { input.readFully(it) }
                pc.close() // Ends a pairing that went ahead, so the phone's thread finishes.
                phone.join(5_000)
                return type to reply
            }
        }
    }

    @Test fun newPcIsRefusedWithoutAskingWhenTheUserDidNotAsk() {
        val asked = AtomicBoolean(false)
        val (type, body) = firstReply(pairingWanted = false, asked = asked)
        assertEquals(WifiHandshake.TYPE_REJECT, type)
        assertEquals(WifiHandshake.REJECT_REFUSED, body.single().toInt())
        assertFalse(asked.get())
    }

    @Test fun forcePairingIsRefusedToo() {
        val asked = AtomicBoolean(false)
        val (type, _) = firstReply(pairingWanted = false, flags = 0x01, asked = asked)
        assertEquals(WifiHandshake.TYPE_REJECT, type)
        assertFalse(asked.get())
    }

    @Test fun newPcGetsThePairingHelloWhenTheUserAsked() {
        val (type, _) = firstReply(pairingWanted = true, asked = AtomicBoolean(false))
        assertEquals(WifiHandshake.TYPE_SERVER_HELLO, type)
        assertNull(pcs.key(ByteArray(16) { 3 })) // Nothing stored: the PC never finished.
    }
}
