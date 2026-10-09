package io.github.whoissaaif.mycam

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Rect
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.SystemClock
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import kotlin.math.roundToInt

/**
 * Streams one camera into a hardware H.264 encoder via its input Surface, at the requested quality and
 * frame rate, with live camera controls (zoom, exposure, torch, focus).
 * All callbacks run on [handler]'s thread; call every method from that thread too.
 */
class CameraStreamer(
    context: Context,
    private val handler: Handler,
    private val facing: Int,
    private var settings: Settings,
    private val listener: Listener,
) {
    /** What the user chose; the phone stores it and the PC can change it (protocol v3). */
    data class Settings(
        val quality: Int = Protocol.QUALITY_1080P,
        val fps: Int = 30,
        val zoom: Float = 1f,
        val ev: Int = 0,
        val torch: Boolean = false,
        val focusLocked: Boolean = false,
    )

    interface Listener {
        /** Codec config (SPS/PPS) is ready. Called before the first frame. */
        fun onConfig(width: Int, height: Int, sensorOrientation: Int, facing: Int, csd: ByteArray)
        fun onFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean)
        fun onError(message: String)
        /** Settings actually in effect and what this camera supports. Sent on start and after each change. */
        fun onCameraInfo(info: Protocol.CameraInfo) {}
        /** Diagnostic text, forwarded to the PC log. */
        fun onLog(message: String) {}
    }

    /** What the opened camera can do. */
    private class Caps(
        val zoomMin: Float, val zoomMax: Float, val useZoomRatio: Boolean, val activeArray: Rect?,
        val evMin: Int, val evMax: Int, val evStep: Float,
        val torch: Boolean, val autofocus: Boolean, val has60: Boolean, val has4K: Boolean,
    )

    private val cameraManager = context.getSystemService(CameraManager::class.java)
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var encoder: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var request: CaptureRequest.Builder? = null
    private var caps: Caps? = null
    private var stopped = false
    private var outputCount = 0
    private var reported = false
    private var configSent = false

    private var width = 0
    private var height = 0
    private var fps = 30
    private var sensorOrientation = 0
    private var realtimeTimestamps = false // Sensor timestamps use elapsedRealtime (else the monotonic clock).
    private var latencySumUs = 0L
    private var latencyCount = 0
    private var lastLatencyLog = 0L

    @SuppressLint("MissingPermission") // Caller checks CAMERA permission before starting.
    fun start() {
        try {
            logCameraReport()
            val cameraId = findCamera() ?: run {
                listener.onError(if (facing == Protocol.FACING_FRONT) "No front camera" else "No back camera")
                return
            }
            val chars = cameraManager.getCameraCharacteristics(cameraId)
            sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
            realtimeTimestamps = chars.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) ==
                CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
            val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)!!
            val sizes = map.getOutputSizes(MediaCodec::class.java) ?: emptyArray()
            val aeRanges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)

            // Highest frame rate the camera can deliver at a size (from its minimum frame duration).
            fun maxFps(s: Size): Int {
                val ns = map.getOutputMinFrameDuration(MediaCodec::class.java, s)
                return if (ns > 0) (1_000_000_000.0 / ns).roundToInt() else 30
            }
            val supports60 = aeRanges?.any { it.upper >= 60 } == true

            val size = chooseSize(sizes, settings.quality) { s -> encoderSupports(s.width, s.height, 30) }
            width = size.width
            height = size.height
            val want60 = settings.fps >= 60 && supports60 && maxFps(size) >= 59 && encoderSupports(width, height, 60)
            fps = if (want60) 60 else 30
            val fpsRange = chooseFpsRange(aeRanges, fps)

            caps = readCaps(chars).let {
                Caps(
                    it.zoomMin, it.zoomMax, it.useZoomRatio, it.activeArray, it.evMin, it.evMax, it.evStep,
                    it.torch, it.autofocus,
                    // Any camera facing this way that can do 60 fps (findCamera switches to it when 60 is asked for).
                    has60 = (supports60 && maxFps(size) >= 59 || sameFacingIds().any { id -> supports60At1080(id) }) &&
                        encoderSupports(minOf(width, 1920), minOf(height, 1080), 60),
                    has4K = sizes.any { s -> s.width == 3840 && s.height == 2160 } && encoderSupports(3840, 2160, 30),
                )
            }
            listener.onLog("camera $cameraId: ${width}x$height @ $fps fps (asked ${settings.fps}), AE $fpsRange, sensor $sensorOrientation")

            startEncoder()
            cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(device: CameraDevice) {
                    if (stopped) { device.close(); return }
                    camera = device
                    listener.onLog("camera opened")
                    createSession(device, fpsRange)
                }

                override fun onDisconnected(device: CameraDevice) {
                    device.close()
                    if (!stopped) listener.onError("Camera disconnected (in use by another app?)")
                }

                override fun onError(device: CameraDevice, error: Int) {
                    device.close()
                    if (!stopped) listener.onError("Camera error $error")
                }
            }, handler)
        } catch (e: Exception) {
            Log.e(TAG, "start failed", e)
            listener.onError("Camera start failed: ${e.message}")
        }
    }

    /** Changes zoom / exposure / torch / focus on the running camera (quality and fps need a restart). */
    fun updateControls(newSettings: Settings) {
        val old = settings
        settings = newSettings
        val s = session ?: return
        val b = request ?: return
        try {
            if (newSettings.focusLocked != old.focusLocked && caps?.autofocus == true) {
                if (newSettings.focusLocked) {
                    // Focus once on the current scene, then hold it.
                    b.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_AUTO)
                    b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_START)
                    s.capture(b.build(), null, handler)
                    b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
                } else {
                    b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_CANCEL)
                    s.capture(b.build(), null, handler)
                    b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
                }
            }
            applyControls(b)
            s.setRepeatingRequest(b.build(), null, handler)
        } catch (e: Exception) {
            listener.onLog("controls not applied: ${e.message}")
        }
        reportInfo()
    }

    fun requestKeyFrame() {
        encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
    }

    fun stop() {
        stopped = true
        try { session?.close() } catch (_: Exception) {}
        try { camera?.close() } catch (_: Exception) {}
        try { encoder?.stop() } catch (_: Exception) {}
        try { encoder?.release() } catch (_: Exception) {}
        inputSurface?.release()
        session = null; camera = null; encoder = null; inputSurface = null; request = null
    }

    /**
     * The first camera facing the right way, unless 60 fps is wanted: then any camera facing that way which
     * can do 60 fps at 1080p (some phones only offer it on a secondary camera id).
     */
    private fun sameFacingIds(): List<String> {
        val wanted = if (facing == Protocol.FACING_FRONT) CameraCharacteristics.LENS_FACING_FRONT
        else CameraCharacteristics.LENS_FACING_BACK
        return cameraManager.cameraIdList.filter {
            cameraManager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == wanted
        }
    }

    private fun findCamera(): String? {
        val ids = sameFacingIds()
        if (settings.fps >= 60) ids.firstOrNull { supports60At1080(it) }?.let { return it }
        return ids.firstOrNull()
    }

    private fun supports60At1080(id: String): Boolean {
        val chars = cameraManager.getCameraCharacteristics(id)
        val ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return false
        val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return false
        val ns = map.getOutputMinFrameDuration(MediaCodec::class.java, Size(1920, 1080))
        return ranges.any { it.upper >= 60 } && ns in 1..17_000_000
    }

    /** One-time report of what every camera offers, so frame-rate limits can be explained from the PC log. */
    private fun logCameraReport() {
        if (reported) return
        reported = true
        for (id in cameraManager.cameraIdList) {
            try {
                val c = cameraManager.getCameraCharacteristics(id)
                val facingName = when (c.get(CameraCharacteristics.LENS_FACING)) {
                    CameraCharacteristics.LENS_FACING_FRONT -> "front"
                    CameraCharacteristics.LENS_FACING_BACK -> "back"
                    else -> "external"
                }
                val map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                val ns = map?.getOutputMinFrameDuration(MediaCodec::class.java, Size(1920, 1080)) ?: 0L
                val max1080 = if (ns > 0) (1_000_000_000.0 / ns).roundToInt() else 0
                val ranges = c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)?.joinToString(" ") ?: "-"
                val caps = c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: IntArray(0)
                val highSpeed = CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO in caps
                val hsSizes = if (highSpeed) map?.highSpeedVideoSizes?.joinToString(" ") { s ->
                    "${s.width}x${s.height}:" + map.getHighSpeedVideoFpsRangesFor(s).joinToString(",")
                } ?: "" else ""
                val physical = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) c.physicalCameraIds.joinToString(",") else ""
                listener.onLog(
                    "camera report $id ($facingName): AE ranges $ranges; 1080p max ${max1080} fps; " +
                        "high-speed ${if (highSpeed) hsSizes else "no"}" + (if (physical.isNotEmpty()) "; physical $physical" else "")
                )
            } catch (e: Exception) {
                listener.onLog("camera report $id: ${e.message}")
            }
        }
    }

    private fun readCaps(chars: CameraCharacteristics): Caps {
        val zoomRatioRange = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
            chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) else null
        val maxDigital = chars.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1f
        val ev = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE) ?: Range(0, 0)
        val evStep = chars.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toFloat() ?: 0f
        val afModes = chars.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: IntArray(0)
        return Caps(
            zoomMin = zoomRatioRange?.lower ?: 1f,
            zoomMax = zoomRatioRange?.upper ?: maxDigital,
            useZoomRatio = zoomRatioRange != null,
            activeArray = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE),
            evMin = ev.lower, evMax = ev.upper, evStep = evStep,
            torch = chars.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true,
            autofocus = CaptureRequest.CONTROL_AF_MODE_AUTO in afModes,
            has60 = false, has4K = false,
        )
    }

    /** Zoom, exposure, torch and focus mode from [settings], clamped to what the camera supports. */
    private fun applyControls(b: CaptureRequest.Builder) {
        val c = caps ?: return
        val zoom = settings.zoom.coerceIn(c.zoomMin, c.zoomMax)
        if (c.useZoomRatio && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            b.set(CaptureRequest.CONTROL_ZOOM_RATIO, zoom)
        } else if (c.activeArray != null) {
            val a = c.activeArray
            val cw = (a.width() / zoom).toInt()
            val ch = (a.height() / zoom).toInt()
            val l = a.left + (a.width() - cw) / 2
            val t = a.top + (a.height() - ch) / 2
            b.set(CaptureRequest.SCALER_CROP_REGION, Rect(l, t, l + cw, t + ch))
        }
        b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, settings.ev.coerceIn(c.evMin, c.evMax))
        b.set(
            CaptureRequest.FLASH_MODE,
            if (settings.torch && c.torch) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF,
        )
        if (c.autofocus) {
            b.set(
                CaptureRequest.CONTROL_AF_MODE,
                if (settings.focusLocked) CaptureRequest.CONTROL_AF_MODE_AUTO else CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO,
            )
        }
    }

    private fun reportInfo() {
        val c = caps ?: return
        var flags = 0
        if (c.torch) flags = flags or Protocol.CAM_TORCH_AVAILABLE
        if (settings.torch && c.torch) flags = flags or Protocol.CAM_TORCH_ON
        if (settings.focusLocked && c.autofocus) flags = flags or Protocol.CAM_FOCUS_LOCKED
        if (c.has60) flags = flags or Protocol.CAM_HAS_60FPS
        if (c.has4K) flags = flags or Protocol.CAM_HAS_4K
        if (c.autofocus) flags = flags or Protocol.CAM_HAS_AUTOFOCUS
        listener.onCameraInfo(
            Protocol.CameraInfo(
                quality = settings.quality, fps = settings.fps,
                zoomX100 = (settings.zoom.coerceIn(c.zoomMin, c.zoomMax) * 100).roundToInt(),
                zoomMinX100 = (c.zoomMin * 100).roundToInt(), zoomMaxX100 = (c.zoomMax * 100).roundToInt().coerceAtMost(65535),
                ev = settings.ev.coerceIn(c.evMin, c.evMax), evMin = c.evMin, evMax = c.evMax,
                evStepX100 = (c.evStep * 100).roundToInt(),
                flags = flags, width = width, height = height, actualFps = fps,
            )
        )
    }

    /**
     * Capture-to-encoded latency on the phone, logged every 5 s. The frame timestamp is the sensor time; which
     * clock that uses varies by phone (the reported source isn't always right), so compare against both
     * candidate clocks and use the one that gives a plausible value.
     */
    private fun measureLatency(ptsUs: Long) {
        val realtime = SystemClock.elapsedRealtimeNanos() / 1000 - ptsUs
        val monotonic = System.nanoTime() / 1000 - ptsUs
        val preferred = if (realtimeTimestamps) realtime else monotonic
        val other = if (realtimeTimestamps) monotonic else realtime
        val latency = if (preferred in 0..2_000_000) preferred else other
        if (latency in 0..2_000_000) {
            latencySumUs += latency
            latencyCount++
        }
        val nowMs = SystemClock.elapsedRealtime()
        if (nowMs - lastLatencyLog >= 5000) {
            if (latencyCount > 0) {
                listener.onLog("latency: capture to encoded avg ${latencySumUs / latencyCount / 1000} ms over $latencyCount frames")
            } else {
                listener.onLog("latency: timestamps not comparable (realtime diff ${realtime / 1000} ms, monotonic ${monotonic / 1000} ms)")
            }
            latencySumUs = 0
            latencyCount = 0
            lastLatencyLog = nowMs
        }
    }

    /** Changes the encoder bitrate live (used to adapt to what the USB link can carry). */
    fun setBitrate(bitsPerSecond: Int) {
        encoder?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bitsPerSecond) })
    }

    /** The bitrate chosen for the current mode, before any adaptation. */
    val targetBitrate: Int get() = bitrateFor(width, height, fps)
    val frameIntervalMs: Int get() = 1000 / fps

    private fun encoderFormat(lowLatencyExtras: Boolean): MediaFormat =
        MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateFor(width, height, fps))
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
            if (lowLatencyExtras) {
                // Keep output steady when the scene is static, and minimise encoder buffering.
                setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 1_000_000L / fps)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) setInteger(MediaFormat.KEY_LATENCY, 1)
                setInteger(MediaFormat.KEY_PRIORITY, 0) // Real-time priority.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
            }
        }

    private fun startEncoder() {
        val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        codec.setCallback(object : MediaCodec.Callback() {
            override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {}

            override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                if (stopped) return
                try {
                    if (outputCount++ == 0) listener.onLog("first encoder output: flags=${info.flags} size=${info.size}")
                    val buf = codec.getOutputBuffer(index)
                    if (buf != null && info.size > 0) {
                        val bytes = ByteArray(info.size)
                        buf.position(info.offset)
                        buf.get(bytes, 0, info.size)
                        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                            listener.onConfig(width, height, sensorOrientation, facing, bytes)
                            configSent = true
                        } else {
                            val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                            if (!configSent) {
                                // No config seen at all: announce the stream anyway; SPS/PPS travel inside key frames.
                                configSent = true
                                listener.onConfig(width, height, sensorOrientation, facing, ByteArray(0))
                            }
                            listener.onFrame(bytes, info.presentationTimeUs, key)
                            measureLatency(info.presentationTimeUs)
                        }
                    }
                    codec.releaseOutputBuffer(index, false)
                } catch (e: IllegalStateException) {
                    // Codec was stopped concurrently; ignore.
                }
            }

            override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                if (!stopped) listener.onError("Encoder error: ${e.diagnosticInfo}")
            }

            override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                // Some encoders (e.g. MediaTek with PREPEND_HEADER_TO_SYNC_FRAMES) never emit a separate
                // CODEC_CONFIG buffer; the output format always carries SPS (csd-0) and PPS (csd-1).
                if (stopped) return
                val csd = listOfNotNull(format.getByteBuffer("csd-0"), format.getByteBuffer("csd-1"))
                    .map { b -> ByteArray(b.remaining()).also { b.duplicate().get(it) } }
                    .fold(ByteArray(0)) { acc, part -> acc + part }
                listener.onLog("encoder format: $format")
                configSent = true
                listener.onConfig(width, height, sensorOrientation, facing, csd)
            }
        }, handler)
        try {
            codec.configure(encoderFormat(lowLatencyExtras = true), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        } catch (e: Exception) {
            // Some vendor encoders reject the optional low-latency keys; retry without them.
            Log.w(TAG, "Encoder rejected format, retrying with basic settings", e)
            codec.reset()
            codec.configure(encoderFormat(lowLatencyExtras = false), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }
        inputSurface = codec.createInputSurface()
        codec.start()
        listener.onLog("encoder ${codec.name} started, ${bitrateFor(width, height, fps) / 1_000_000} Mbps")
        encoder = codec
    }

    @Suppress("DEPRECATION") // List<Surface> overload works on all supported API levels.
    private fun createSession(device: CameraDevice, fpsRange: Range<Int>) {
        val surface = inputSurface ?: return
        device.createCaptureSession(listOf(surface), object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(s: CameraCaptureSession) {
                if (stopped) { s.close(); return }
                session = s
                val b = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                    addTarget(surface)
                    set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)
                    set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                }
                applyControls(b)
                request = b
                try {
                    s.setRepeatingRequest(b.build(), null, handler)
                    listener.onLog("capture started")
                    reportInfo()
                } catch (e: Exception) {
                    listener.onError("Capture failed: ${e.message}")
                }
            }

            override fun onConfigureFailed(s: CameraCaptureSession) {
                listener.onError("Camera session configuration failed")
            }
        }, handler)
    }

    companion object {
        private const val TAG = "CameraStreamer"

        fun targetSize(quality: Int): Size = when (quality) {
            Protocol.QUALITY_720P -> Size(1280, 720)
            Protocol.QUALITY_4K -> Size(3840, 2160)
            else -> Size(1920, 1080)
        }

        /** About 0.16 bits per pixel per frame: 4 Mbps at 720p30, 10 at 1080p30, 20 at 1080p60, 40 at 4K30. */
        fun bitrateFor(width: Int, height: Int, fps: Int): Int =
            (width.toLong() * height * fps * 16 / 100).coerceIn(4_000_000L, 40_000_000L).toInt()

        /**
         * The requested quality if the camera and encoder can do it exactly; otherwise the largest 16:9 size
         * up to it, then the largest size up to it, then the smallest available.
         */
        fun chooseSize(sizes: Array<Size>, quality: Int, encoderOk: (Size) -> Boolean = { true }): Size {
            val target = targetSize(quality)
            val usable = sizes.filter { encoderOk(it) }.ifEmpty { sizes.toList() }
            val capped = usable.filter { it.width <= target.width && it.height <= target.height }
            capped.firstOrNull { it.width == target.width && it.height == target.height }?.let { return it }
            capped.filter { it.width * 9 == it.height * 16 }.maxByOrNull { it.width * it.height }?.let { return it }
            capped.maxByOrNull { it.width * it.height }?.let { return it }
            return usable.minByOrNull { it.width * it.height } ?: Size(1280, 720)
        }

        /** A fixed [fps] range if available, else the range reaching [fps] with the highest minimum, else <= [fps]. */
        fun chooseFpsRange(ranges: Array<Range<Int>>?, fps: Int): Range<Int> {
            if (ranges.isNullOrEmpty()) return Range(fps, fps)
            ranges.firstOrNull { it.lower == fps && it.upper == fps }?.let { return it }
            ranges.filter { it.upper == fps }.maxByOrNull { it.lower }?.let { return it }
            return ranges.filter { it.upper <= fps }.maxWithOrNull(compareBy({ it.upper }, { it.lower }))
                ?: ranges.first()
        }

        private val encoderCache = HashMap<Triple<Int, Int, Int>, Boolean>()

        /** Whether a hardware/software AVC encoder on this phone accepts this size and frame rate. */
        fun encoderSupports(width: Int, height: Int, fps: Int): Boolean = encoderCache.getOrPut(Triple(width, height, fps)) {
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            }
            try { MediaCodecList(MediaCodecList.REGULAR_CODECS).findEncoderForFormat(format) != null } catch (_: Exception) { false }
        }
    }
}
