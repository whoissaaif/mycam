package io.github.whoissaaif.mycam

import org.junit.Assert.assertEquals
import org.junit.Test

/** The same cases as web::CleanName on the PC (pc/tests/tests.cpp). */
class PcNamesTest {
    private fun clean(vararg bytes: Int) = PcNames.clean(ByteArray(bytes.size) { bytes[it].toByte() }, "x")
    private fun ascii(s: String) = s.toByteArray().map { it.toInt() }.toIntArray()

    @Test fun ordinaryNamesAreKept() {
        assertEquals("Pixel 9", PcNames.clean("Pixel 9", "fallback"))
        assertEquals("Café ☕", PcNames.clean("Café ☕", "x"))
        assertEquals("A📷B", PcNames.clean("A📷B", "x")) // An emoji (4 bytes).
        assertEquals("A B", PcNames.clean("A B", "x"))            // Just after C1.
    }

    @Test fun emptyFallsBack() {
        assertEquals("192.168.1.9", PcNames.clean("", "192.168.1.9"))
        assertEquals("fallback", PcNames.clean("  \t ", "fallback"))
        assertEquals("iPad", PcNames.clean("  iPad\t", "x"))
    }

    @Test fun nothingLeavesItsLine() {
        assertEquals("AAllow this", PcNames.clean("A\nAllow this", "x"))
        assertEquals("AB", PcNames.clean("A\r\nB", "x"))
        assertEquals("AB", PcNames.clean("A\u007FB", "x"))
        assertEquals("AB", PcNames.clean("A B", "x"))
        assertEquals("AB", PcNames.clean("A B", "x"))
        assertEquals("AB", PcNames.clean("A\u0085B", "x"))       // NEL.
        assertEquals("AB", PcNames.clean("A\u0080\u009FB", "x")) // The rest of C1.
    }

    @Test fun bidiControlsGo() {
        assertEquals("AB", PcNames.clean("A‮B", "x"))
        assertEquals("ABC", PcNames.clean("A⁦B⁩C", "x"))
        assertEquals("AB", PcNames.clean("A‎‏B", "x"))
        assertEquals("AB", PcNames.clean("A؜B", "x"))
        assertEquals("AB", PcNames.clean("A﻿B", "x"))
    }

    @Test fun brokenUtf8Stops() {
        assertEquals("ab", clean('a'.code, 'b'.code, 0xFF, 'c'.code, 'd'.code))
        assertEquals("A", clean('A'.code, 0xC0, 0x8A, 'B'.code))             // Overlong newline.
        assertEquals("A", clean('A'.code, 0xE0, 0x80, 0xAF, 'B'.code))       // Overlong '/'.
        assertEquals("A", clean('A'.code, 0xF0, 0x80, 0x80, 0xAF, 'B'.code)) // Overlong, 4 bytes.
        assertEquals("A", clean('A'.code, 0xED, 0xA0, 0x80, 'B'.code))       // A surrogate half.
        assertEquals("A", clean('A'.code, 0xF4, 0x90, 0x80, 0x80, 'B'.code)) // Past U+10FFFF.
        assertEquals("A", clean('A'.code, 0xE2, 0x80))                       // Cut short.
        assertEquals("x", clean(0xC0, 0x8A))                                 // Nothing valid: the fallback.
        assertEquals("ok", clean(*ascii("ok")))
    }

    @Test fun cappedAtWholeCharacters() {
        assertEquals(40, PcNames.clean("x".repeat(60), "y").length)
        // 39 ASCII bytes and a 2-byte character: the character doesn't fit and isn't cut in half.
        assertEquals("x".repeat(39), PcNames.clean("x".repeat(39) + "é", "y"))
    }
}
