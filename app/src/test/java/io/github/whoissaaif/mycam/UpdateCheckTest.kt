package io.github.whoissaaif.mycam

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Reading GitHub's release reply and deciding what counts as newer (#79). The same cases as the PC's
 * TestUpdateVersion in pc/tests/tests.cpp, so both apps agree on what an update is.
 */
class UpdateCheckTest {
    @Test
    fun `tag comes out of a release reply`() {
        val reply = """{"url":"https://api.github.com/repos/whoissaaif/mycam/releases/1",""" +
            """"tag_name":"v1.7.0","name":"MyCam 1.7.0","draft":false}"""
        assertEquals("1.7.0", UpdateCheck.latestTagFromJson(reply))
        assertEquals("1.6.1", UpdateCheck.latestTagFromJson("""{"tag_name": "1.6.1"}"""))
        assertEquals("", UpdateCheck.latestTagFromJson("""{"message":"Not Found"}"""))
        assertEquals("", UpdateCheck.latestTagFromJson(""))
        assertEquals("", UpdateCheck.latestTagFromJson("""{"tag_name":""")) // Cut off mid-reply.
    }

    @Test
    fun `newer wins, part by part`() {
        assertTrue(UpdateCheck.isNewerVersion("1.6.1", "1.6.0"))
        assertTrue(UpdateCheck.isNewerVersion("1.7.0", "1.6.9"))
        assertTrue(UpdateCheck.isNewerVersion("2.0.0", "1.9.9"))
        // Numbers, never text: "10" beats "9".
        assertTrue(UpdateCheck.isNewerVersion("1.10.0", "1.9.0"))
        assertTrue(UpdateCheck.isNewerVersion("1.6.10", "1.6.9"))
        assertFalse(UpdateCheck.isNewerVersion("1.6.0", "1.6.0"))
        assertFalse(UpdateCheck.isNewerVersion("1.6.0", "1.6.1"))
        assertFalse(UpdateCheck.isNewerVersion("1.5.9", "1.6.0"))
        assertFalse(UpdateCheck.isNewerVersion("0.9.9", "1.0.0"))
    }

    @Test
    fun `a pre-release loses to the release with the same numbers`() {
        assertTrue(UpdateCheck.isNewerVersion("1.6.0", "1.6.0-beta4"))
        assertFalse(UpdateCheck.isNewerVersion("1.6.0-beta4", "1.6.0"))
        assertTrue(UpdateCheck.isNewerVersion("1.7.0-beta1", "1.6.0"))
    }

    @Test
    fun `anything unreadable offers no update`() {
        assertFalse(UpdateCheck.isNewerVersion("", "1.6.0"))
        assertFalse(UpdateCheck.isNewerVersion("1.6", "1.6.0"))
        assertFalse(UpdateCheck.isNewerVersion("nightly", "1.6.0"))
        assertFalse(UpdateCheck.isNewerVersion("9.9.9", ""))
        assertFalse(UpdateCheck.isNewerVersion("99999999.0.0", "1.6.0")) // Absurd, so refused, not overflowed.
    }
}
