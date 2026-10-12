package io.github.whoissaaif.mycam

import android.annotation.SuppressLint
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.net.URI
import java.net.URL
import java.net.URLDecoder
import java.net.URLEncoder
import java.security.MessageDigest
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLException
import javax.net.ssl.X509TrustManager

/**
 * Pairing by scanning the PC's QR code (PROTOCOL.md "QR pairing"): the same code iPhones scan, read here
 * natively. The PC is reached over HTTPS pinned to the certificate fingerprint in the code, hands back a
 * Wi-Fi pairing key, and then connects to this phone as to any paired phone. No code to compare, nothing
 * to allow: scanning the PC's screen is the consent.
 *
 * The parsing and the crypto values are plain Kotlin (checked against protocol/golden.txt "qr.*"); the two
 * requests block, so call them off the main thread.
 */
object QrPairing {
    const val PC_PORT = 47900

    /** What a MyCam QR code holds: the PC's address, the one-time code and the certificate fingerprint. */
    class Code(val host: String, val code: String, val fingerprint: ByteArray)

    sealed class Scan {
        /** Not a MyCam code at all: the scanner keeps looking and says nothing. */
        object NotMyCam : Scan()
        /** A MyCam code from a companion before 1.5 (no fingerprint): MyCam on the PC needs updating. */
        object OldPc : Scan()
        class Ok(val code: Code) : Scan()
    }

    /** Why a pairing didn't work, in terms the screen can explain. NoSession: paired, but the PC never connected. */
    enum class Failure { WrongPc, CodeUsed, Unreachable, Busy, Forgotten, NoSession, Other }

    class PairingException(val failure: Failure, message: String, cause: Throwable? = null) : IOException(message, cause)

    /** Reads `https://<ipv4>:47900/?t=<code>&f=<fingerprint>`. */
    fun parse(text: String): Scan {
        val uri = try { URI(text.trim()) } catch (_: Exception) { return Scan.NotMyCam }
        if (!"https".equals(uri.scheme, ignoreCase = true) || uri.port != PC_PORT) return Scan.NotMyCam
        if (!(uri.rawPath.isNullOrEmpty() || uri.rawPath == "/")) return Scan.NotMyCam
        val host = uri.host ?: return Scan.NotMyCam
        val query = queryOf(uri.rawQuery ?: "")
        val code = query["t"]?.takeIf { it.isNotEmpty() } ?: return Scan.NotMyCam
        val fingerprint = query["f"]?.let { decodeBase64Url(it) } ?: return Scan.OldPc
        if (fingerprint.size != 32) return Scan.OldPc
        return Scan.Ok(Code(host, code, fingerprint))
    }

    /** What the QR code's `f` holds for a certificate: base64url of SHA-256 over its DER bytes. */
    fun fingerprint(der: ByteArray): String = encodeBase64Url(MessageDigest.getInstance("SHA-256").digest(der))

    /** HMAC(K, "MyCam connect v1" | phone id | port, u16 BE), sent with /app/connect. */
    fun connectProof(key: ByteArray, phoneId: ByteArray, port: Int): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        mac.update("MyCam connect v1".toByteArray(Charsets.US_ASCII))
        mac.update(phoneId)
        mac.update(byteArrayOf((port shr 8).toByte(), port.toByte()))
        return mac.doFinal()
    }

    /** The PC this phone just paired with. [pcId] is 16 bytes; [key] is the pairing key K. */
    class Paired(val pcId: ByteArray, val pcName: String, val key: ByteArray)

    /** POST /app/pair: trades the one-time code for a pairing key. Blocks; throws [PairingException]. */
    fun pair(code: Code, phoneId: ByteArray, phoneName: String): Paired {
        val json = post(code.host, code.fingerprint, "/app/pair", mapOf("t" to code.code, "phone" to hex(phoneId), "name" to phoneName))
        val pcId = json.optString("pc").let { unhex(it) }
        val key = decodeBase64Url(json.optString("key"))
        if (pcId == null || pcId.size != 16 || key == null || key.size != 32) {
            throw PairingException(Failure.Other, "the PC's answer doesn't make sense")
        }
        return Paired(pcId, json.optString("name").ifEmpty { code.host }, key)
    }

    /** POST /app/connect: asks the paired PC to open a session to this phone now. Blocks; throws [PairingException]. */
    fun connect(host: String, fingerprint: ByteArray, phoneId: ByteArray, key: ByteArray, tcpPort: Int) {
        post(
            host, fingerprint, "/app/connect",
            mapOf("phone" to hex(phoneId), "port" to tcpPort.toString(), "proof" to encodeBase64Url(connectProof(key, phoneId, tcpPort))),
        )
    }

    // --- HTTPS, pinned to the fingerprint ---------------------------------------------------------------

    private class PinMismatch : CertificateException("not the certificate in the QR code")

    // A custom trust manager on purpose: it trusts exactly one certificate, the one whose SHA-256 the QR code
    // carries (certificate pinning), and nothing else, not even the system CAs.
    @SuppressLint("CustomX509TrustManager")
    private fun post(host: String, fingerprint: ByteArray, path: String, form: Map<String, String>): JSONObject {
        val trust = object : X509TrustManager {
            override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) = throw CertificateException()
            override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
                val leaf = chain?.firstOrNull() ?: throw PinMismatch()
                val digest = MessageDigest.getInstance("SHA-256").digest(leaf.encoded)
                // Exactly the PC whose screen was scanned: no CA, no name check (it is self-signed and
                // reached by address), only the pin.
                if (!MessageDigest.isEqual(digest, fingerprint)) throw PinMismatch()
            }
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
        }
        val tls = SSLContext.getInstance("TLS").apply { init(null, arrayOf(trust), null) }
        val body = form.entries.joinToString("&") { (k, v) -> k + "=" + URLEncoder.encode(v, "UTF-8") }.toByteArray()
        val conn = URL("https", host, PC_PORT, path).openConnection() as HttpsURLConnection
        try {
            conn.sslSocketFactory = tls.socketFactory
            conn.hostnameVerifier = javax.net.ssl.HostnameVerifier { h, _ -> h == host } // The pin did the real check.
            conn.connectTimeout = 5_000
            conn.readTimeout = 10_000
            conn.requestMethod = "POST"
            conn.doOutput = true
            conn.useCaches = false
            conn.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
            conn.setFixedLengthStreamingMode(body.size)
            conn.outputStream.use { it.write(body) }
            val status = conn.responseCode
            val text = (if (status in 200..299) conn.inputStream else conn.errorStream)?.use { it.readBytes().toString(Charsets.UTF_8) } ?: ""
            when (status) {
                HttpURLConnection.HTTP_OK -> return try { JSONObject(text) } catch (e: Exception) {
                    throw PairingException(Failure.Other, "the PC's answer isn't JSON", e)
                }
                HttpURLConnection.HTTP_FORBIDDEN ->
                    throw if (path == "/app/pair") PairingException(Failure.CodeUsed, "the code is used or expired")
                    else PairingException(Failure.Forgotten, "the PC doesn't know this phone any more")
                HttpURLConnection.HTTP_UNAVAILABLE -> throw PairingException(Failure.Busy, "another phone is using the PC")
                else -> throw PairingException(Failure.Other, "the PC answered $status")
            }
        } catch (e: PairingException) {
            throw e
        } catch (e: SSLException) {
            throw if (causedBy<PinMismatch>(e)) PairingException(Failure.WrongPc, "the certificate isn't the one in the QR code", e)
            else PairingException(Failure.Unreachable, "TLS failed: ${e.message}", e)
        } catch (e: SocketTimeoutException) {
            throw PairingException(Failure.Unreachable, "the PC didn't answer", e)
        } catch (e: IOException) {
            throw PairingException(Failure.Unreachable, "can't reach the PC: ${e.message}", e)
        } finally {
            conn.disconnect()
        }
    }

    private inline fun <reified T : Throwable> causedBy(e: Throwable): Boolean {
        var c: Throwable? = e
        while (c != null) {
            if (c is T) return true
            c = c.cause
        }
        return false
    }

    // --- Encodings (android.util.Base64 isn't there in unit tests, java.util.Base64 needs API 26) --------

    private const val ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"

    fun encodeBase64Url(b: ByteArray): String {
        val out = StringBuilder((b.size + 2) / 3 * 4)
        var i = 0
        while (i < b.size) {
            val v = (b[i].toInt() and 0xFF shl 16) or
                ((if (i + 1 < b.size) b[i + 1].toInt() and 0xFF else 0) shl 8) or
                (if (i + 2 < b.size) b[i + 2].toInt() and 0xFF else 0)
            out.append(ALPHABET[v shr 18 and 63]).append(ALPHABET[v shr 12 and 63])
            if (i + 1 < b.size) out.append(ALPHABET[v shr 6 and 63])
            if (i + 2 < b.size) out.append(ALPHABET[v and 63])
            i += 3
        }
        return out.toString()
    }

    /** Base64url without padding; null if it isn't exactly that. */
    fun decodeBase64Url(s: String): ByteArray? {
        if (s.length % 4 == 1) return null
        val out = java.io.ByteArrayOutputStream(s.length * 3 / 4)
        var acc = 0
        var bits = 0
        for (c in s) {
            val v = ALPHABET.indexOf(c)
            if (v < 0) return null
            acc = (acc shl 6) or v
            bits += 6
            if (bits >= 8) {
                bits -= 8
                out.write(acc shr bits and 0xFF)
            }
        }
        if (acc and ((1 shl bits) - 1) != 0) return null
        return out.toByteArray()
    }

    fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }

    fun unhex(s: String): ByteArray? {
        if (s.length % 2 != 0) return null
        return try { ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() } } catch (_: NumberFormatException) { null }
    }

    private fun queryOf(raw: String): Map<String, String> = raw.split('&').filter { it.isNotEmpty() }.associate { pair ->
        val eq = pair.indexOf('=')
        val name = if (eq < 0) pair else pair.substring(0, eq)
        val value = if (eq < 0) "" else pair.substring(eq + 1)
        URLDecoder.decode(name, "UTF-8") to URLDecoder.decode(value, "UTF-8")
    }
}
