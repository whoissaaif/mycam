package io.github.whoissaaif.mycam

import android.content.SharedPreferences
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.net.HttpURLConnection
import java.net.URL

/**
 * "Version X is available" on the About page (issue #79). One HTTPS GET of this project's latest GitHub
 * release, at most once a day, and nothing is sent beyond the request itself. One switch turns it off; the
 * answer is remembered in the prefs, so the About page shows what the last check found without waiting.
 *
 * The same rules as the PC's pc/companion/update_check.cpp, including which versions count as newer.
 */
object UpdateCheck {
    /** Where "Version X is available" goes, and what the check asks GitHub for. */
    private const val API_URL = "https://api.github.com/repos/whoissaaif/mycam/releases/latest"
    private const val PREF_ON = "check_updates"
    private const val PREF_LATEST = "update_latest"
    private const val PREF_CHECKED_AT = "update_checked_at"
    private const val CHECK_EVERY_MS = 24L * 60 * 60 * 1000
    private const val TIMEOUT_MS = 8000
    private const val MAX_BODY = 256 * 1024

    /** On unless the user turned it off. */
    fun enabled(prefs: SharedPreferences): Boolean = prefs.getBoolean(PREF_ON, true)

    fun setEnabled(prefs: SharedPreferences, on: Boolean) {
        val edit = prefs.edit().putBoolean(PREF_ON, on)
        // Don't keep offering a version we may no longer check for.
        if (!on) edit.remove(PREF_LATEST)
        edit.apply()
    }

    /** The newer release to offer, or null: nothing newer, never checked, or turned off. */
    fun available(prefs: SharedPreferences, currentVersion: String): String? {
        if (!enabled(prefs)) return null
        val latest = prefs.getString(PREF_LATEST, null) ?: return null
        return if (isNewerVersion(latest, currentVersion)) latest else null
    }

    /**
     * One check, if it is on and the last one was over a day ago. A failed check stores nothing, so the
     * next time the page opens tries again.
     */
    suspend fun refresh(prefs: SharedPreferences, currentVersion: String) {
        if (!enabled(prefs)) return
        val checkedAt = prefs.getLong(PREF_CHECKED_AT, 0)
        val now = System.currentTimeMillis()
        // now < checkedAt: the clock went backwards, so check again rather than wait for it to catch up.
        if (checkedAt > 0 && now >= checkedAt && now - checkedAt < CHECK_EVERY_MS) return
        val body = withContext(Dispatchers.IO) { get(API_URL) } ?: return
        val tag = latestTagFromJson(body)
        if (tag.isEmpty()) return
        prefs.edit().putLong(PREF_CHECKED_AT, now).putString(PREF_LATEST, tag).apply()
        if (isNewerVersion(tag, currentVersion)) Log.i("MyCam", "update check: $tag is available")
    }

    /** The reply body, or null if the request failed or the reply wasn't a 200. */
    private fun get(url: String): String? = try {
        val connection = URL(url).openConnection() as HttpURLConnection
        try {
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            // "MyCam" and nothing else: the GitHub API needs a User-Agent, and this sends no version or phone.
            connection.setRequestProperty("User-Agent", "MyCam")
            connection.setRequestProperty("Accept", "application/vnd.github+json")
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                null
            } else {
                val body = connection.inputStream.bufferedReader().use { it.readText() }
                if (body.length > MAX_BODY) null else body // A release reply is a few KB.
            }
        } finally {
            connection.disconnect()
        }
    } catch (_: Exception) {
        null // No network, no DNS, GitHub saying no: try again next time, and say nothing to the user.
    }

    /** The "tag_name" of a GitHub release reply, without a leading "v" ("" if the reply hasn't got one). */
    fun latestTagFromJson(json: String): String {
        // shortcut: finds the first "tag_name" rather than parsing the JSON, which is enough for the one
        // object the releases/latest endpoint returns. Parse it properly if we ever read a list of releases.
        val key = json.indexOf("\"tag_name\"")
        if (key < 0) return ""
        val colon = json.indexOf(':', key)
        if (colon < 0) return ""
        val open = json.indexOf('"', colon)
        if (open < 0) return ""
        val close = json.indexOf('"', open + 1)
        if (close < 0) return ""
        return json.substring(open + 1, close).removePrefix("v").removePrefix("V")
    }

    /**
     * True if release [latest] comes after [current] ("1.7.0" after "1.6.1"). Both are MAJOR.MINOR.PATCH,
     * optionally with a pre-release suffix, which loses to the same numbers without one ("1.6.0" is newer
     * than "1.6.0-beta4"). Anything else answers false: never offer an update we can't read.
     */
    fun isNewerVersion(latest: String, current: String): Boolean {
        val a = parse(latest) ?: return false
        val b = parse(current) ?: return false
        for (i in 0..2) if (a.parts[i] != b.parts[i]) return a.parts[i] > b.parts[i]
        return b.suffix && !a.suffix // Same numbers: only the stable release beats a pre-release of it.
    }

    private class Version(val parts: IntArray, val suffix: Boolean)

    /** "1.6.0" to {1, 6, 0}; suffix is set for anything after the patch number ("-beta4", "+build7"). */
    private fun parse(v: String): Version? {
        val parts = IntArray(3)
        var i = 0
        for (part in 0..2) {
            if (i >= v.length || !v[i].isDigit()) return null
            var n = 0
            while (i < v.length && v[i].isDigit()) {
                if (n > 99999) return null // Not a version of ours, and the next digit would overflow.
                n = n * 10 + (v[i++] - '0')
            }
            parts[part] = n
            if (part < 2) {
                if (i >= v.length || v[i] != '.') return null
                i++
            }
        }
        return Version(parts, i < v.length)
    }
}
