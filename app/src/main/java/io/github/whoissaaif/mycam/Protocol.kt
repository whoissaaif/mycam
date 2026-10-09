package io.github.whoissaaif.mycam

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Wire protocol shared with the Windows companion (see pc/companion/protocol.h).
 *
 * Phone -> PC: 20-byte big-endian header followed by `length` payload bytes.
 *   u32 magic 'MCAM' | u8 type | u8 flags | u16 reserved | i64 ptsUs | u32 length
 *
 * PC -> Phone: fixed 8-byte commands.
 *   u32 magic 'MCMD' | u8 cmd | u8 arg | u16 reserved
 */
object Protocol {
    const val VERSION = 2

    const val PACKET_MAGIC = 0x4D43414D // 'MCAM'
    const val HEADER_SIZE = 20

    // Phone -> PC packet types
    const val TYPE_HELLO = 0   // payload: u16 protocol version
    const val TYPE_CONFIG = 1  // payload: u16 width, u16 height, u16 sensorOrientation, u8 facing, then SPS/PPS (Annex-B)
    const val TYPE_FRAME = 2   // payload: H.264 Annex-B access unit
    const val TYPE_ORIENT = 3  // payload: u16 device rotation in degrees (0/90/180/270, clockwise)
    const val TYPE_STATE = 4   // payload: u8 state (STATE_*), u8 facing
    const val TYPE_LOG = 5     // payload: UTF-8 diagnostic text (shown in the PC log)

    const val FLAG_KEYFRAME = 0x01

    const val STATE_IDLE = 0
    const val STATE_STREAMING = 1
    const val STATE_ERROR = 2
    const val STATE_PAUSED = 3 // v2: camera off because the user paused (on the phone or the PC)

    const val FACING_BACK = 0
    const val FACING_FRONT = 1

    const val COMMAND_MAGIC = 0x4D434D44 // 'MCMD'
    const val COMMAND_SIZE = 8

    // PC -> Phone commands
    const val CMD_HELLO = 1
    const val CMD_START = 2
    const val CMD_STOP = 3
    const val CMD_KEYFRAME = 4
    const val CMD_SET_FACING = 5 // arg: FACING_*
    const val CMD_PAUSE = 6 // v2
    const val CMD_RESUME = 7 // v2

    fun packet(type: Int, flags: Int, ptsUs: Long, payload: ByteArray, offset: Int = 0, length: Int = payload.size): ByteArray {
        val buf = ByteBuffer.allocate(HEADER_SIZE + length).order(ByteOrder.BIG_ENDIAN)
        buf.putInt(PACKET_MAGIC)
        buf.put(type.toByte())
        buf.put(flags.toByte())
        buf.putShort(0)
        buf.putLong(ptsUs)
        buf.putInt(length)
        buf.put(payload, offset, length)
        return buf.array()
    }

    data class Command(val cmd: Int, val arg: Int)

    /** Splits the PC's byte stream into commands, skipping garbage until the next valid magic. */
    class CommandParser {
        private var pending = ByteArray(0)

        fun feed(bytes: ByteArray, length: Int = bytes.size): List<Command> {
            val data = pending + bytes.copyOfRange(0, length)
            val buf = ByteBuffer.wrap(data).order(ByteOrder.BIG_ENDIAN)
            val commands = mutableListOf<Command>()
            var pos = 0
            while (data.size - pos >= COMMAND_SIZE) {
                // Only accept plausible commands so stray magic bytes in garbage can't desync the stream.
                val cmd = data[pos + 4].toInt() and 0xFF
                val valid = buf.getInt(pos) == COMMAND_MAGIC && cmd in CMD_HELLO..CMD_RESUME &&
                    data[pos + 6].toInt() == 0 && data[pos + 7].toInt() == 0
                if (!valid) {
                    pos++ // Resync byte by byte.
                    continue
                }
                commands += Command(cmd, data[pos + 5].toInt() and 0xFF)
                pos += COMMAND_SIZE
            }
            pending = data.copyOfRange(pos, data.size)
            return commands
        }
    }
}
