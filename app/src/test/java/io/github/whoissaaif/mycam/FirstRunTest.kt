package io.github.whoissaaif.mycam

import android.content.Context
import androidx.core.content.edit
import io.github.whoissaaif.mycam.ui.FirstRun
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

/** The first-run cards are shown once, to a new user (Skip / Got it / first connection end them for good). */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class FirstRunTest {
    private val prefs = RuntimeEnvironment.getApplication().getSharedPreferences(WebcamService.PREFS, Context.MODE_PRIVATE)

    @Test fun newUserSeesItUntilDone() {
        assertTrue(FirstRun.shouldShow(prefs))
        assertTrue(FirstRun.shouldShow(prefs)) // A relaunch without finishing still shows it.
        FirstRun.done(prefs)
        assertFalse(FirstRun.shouldShow(prefs))
    }

    @Test fun upgradedUserWithPairedPcNeverSeesIt() {
        PairedPcs(prefs).put(ByteArray(16) { 1 }, ByteArray(32), "DESKTOP-ABC")
        assertFalse(FirstRun.shouldShow(prefs))
    }

    @Test fun previousConnectionFlagHidesIt() {
        prefs.edit(commit = true) { putBoolean(WebcamService.PREF_FIRST_RUN_DONE, true) }
        assertFalse(FirstRun.shouldShow(prefs))
    }
}
