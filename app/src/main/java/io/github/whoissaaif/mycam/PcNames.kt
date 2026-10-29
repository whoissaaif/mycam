package io.github.whoissaaif.mycam

import java.io.ByteArrayOutputStream

/**
 * Cleans a name a PC chose for itself before the phone shows it (pairing question, notification, paired
 * list). The same rules as the PC's `web::CleanName` (pc/companion/web_proto.cpp): nothing in a name may
 * leave its own line or turn the text around it, so a crafted name can't rearrange the pairing question.
 */
object PcNames {
    /** At most this many UTF-8 bytes are kept (whole characters only), as on the PC. */
    const val MAX_BYTES = 40

    /**
     * Drops control characters (C0, DEL and C1, U+0085 among them), line and paragraph separators, and
     * bidirectional marks, overrides and isolates; stops at anything that isn't UTF-8 (overlong forms and
     * surrogates included); keeps up to [MAX_BYTES]; trims spaces and tabs. Empty: [fallback].
     */
    fun clean(name: ByteArray, fallback: String): String {
        val out = ByteArrayOutputStream()
        var i = 0
        while (i < name.size) {
            val c = name[i].toInt() and 0xFF
            val len = when {
                c < 0x80 -> 1
                c shr 5 == 6 -> 2
                c shr 4 == 14 -> 3
                c shr 3 == 30 -> 4
                else -> 0
            }
            if (len == 0 || i + len > name.size) break // Not UTF-8: stop here.
            if ((1 until len).any { (name[i + it].toInt() and 0xC0) != 0x80 }) break
            if (len == 1) {
                if (c < 0x20 || c == 0x7F) { i += 1; continue } // Control characters go.
            } else {
                var cp = when (len) { 2 -> c and 0x1F; 3 -> c and 0x0F; else -> c and 0x07 }
                for (k in 1 until len) cp = (cp shl 6) or (name[i + k].toInt() and 0x3F)
                // Overlong forms (C0 8A is a disguised newline), surrogates and code points past U+10FFFF.
                val least = when (len) { 2 -> 0x80; 3 -> 0x800; else -> 0x10000 }
                if (cp < least || cp in 0xD800..0xDFFF || cp > 0x10FFFF) break
                if (unshowable(cp)) { i += len; continue }
            }
            if (out.size() + len > MAX_BYTES) break
            out.write(name, i, len)
            i += len
        }
        val text = String(out.toByteArray(), Charsets.UTF_8).trim(' ', '\t')
        return text.ifEmpty { fallback }
    }

    fun clean(name: String, fallback: String): String = clean(name.toByteArray(Charsets.UTF_8), fallback)

    private fun unshowable(cp: Int) =
        cp in 0x80..0x9F ||                 // C1 controls, NEL among them.
            cp == 0x061C ||                 // Arabic letter mark.
            cp == 0x2028 || cp == 0x2029 || // Line / paragraph separator.
            cp == 0x200E || cp == 0x200F || // Left-to-right / right-to-left mark.
            cp in 0x202A..0x202E ||         // Embedding and override.
            cp in 0x2066..0x2069 ||         // Isolates.
            cp == 0xFEFF                    // Zero-width no-break space.
}
