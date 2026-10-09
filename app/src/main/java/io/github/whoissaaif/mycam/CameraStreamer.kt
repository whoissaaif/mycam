package io.github.whoissaaif.mycam

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface

/**
 * Streams one camera into a hardware H.264 encoder via its input Surface.
 * All callbacks run on [handler]'s thread; call [start]/[stop] from that thread too.
 */
class CameraStreamer(
    context: Context,
    private val handler: Handler,
    private val facing: Int,
    private val listener: Listener,
) {
    interface Listener {
        /** Codec config (SPS/PPS) is ready. Called before the first frame. */
        fun onConfig(width: Int, height: Int, sensorOrientation: Int, facing: Int, csd: ByteArray)
        fun onFrame(data: ByteArray, ptsUs: Long, keyFrame: Boolean)
        fun onError(message: String)
        /** Diagnostic text, forwarded to the PC log. */
        fun onLog(message: String) {}
    }

    private val cameraManager = context.getSystemService(CameraManager::class.java)
    private var camera: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var encoder: MediaCodec? = null
    private var inputSurface: Surface? = null
    private var stopped = false
    private var outputCount = 0
    private var configSent = false

    private var width = 0
    private var height = 0
    private var sensorOrientation = 0

    @SuppressLint("MissingPermission") // Caller checks CAMERA permission before starting.
    fun start() {
        try {
            val cameraId = findCamera() ?: run {
                listener.onError(if (facing == Protocol.FACING_FRONT) "No front camera" else "No back camera")
                return
            }
            val chars = cameraManager.getCameraCharacteristics(cameraId)
            sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
            val map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)!!
            val size = chooseSize(map.getOutputSizes(MediaCodec::class.java) ?: emptyArray())
            width = size.width
            height = size.height
            val fpsRange = chooseFpsRange(chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES))
            listener.onLog("camera $cameraId: ${width}x$height, fps $fpsRange, sensor $sensorOrientation")

            startEncoder(fpsRange.upper.coerceAtMost(30))
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
        session = null; camera = null; encoder = null; inputSurface = null
    }

    private fun findCamera(): String? {
        val wanted = if (facing == Protocol.FACING_FRONT) CameraCharacteristics.LENS_FACING_FRONT
        else CameraCharacteristics.LENS_FACING_BACK
        return cameraManager.cameraIdList.firstOrNull {
            cameraManager.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == wanted
        }
    }

    private fun encoderFormat(fps: Int, lowLatencyExtras: Boolean): MediaFormat =
        MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, if (width * height >= 1920 * 1080) 10_000_000 else 6_000_000)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
            if (lowLatencyExtras) {
                // Keep output steady when the scene is static, and minimise encoder buffering.
                setLong(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, 1_000_000L / fps)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) setInteger(MediaFormat.KEY_LATENCY, 1)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) setInteger(MediaFormat.KEY_PREPEND_HEADER_TO_SYNC_FRAMES, 1)
            }
        }

    private fun startEncoder(fps: Int) {
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
            codec.configure(encoderFormat(fps, lowLatencyExtras = true), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        } catch (e: Exception) {
            // Some vendor encoders reject the optional low-latency keys; retry without them.
            Log.w(TAG, "Encoder rejected format, retrying with basic settings", e)
            codec.reset()
            codec.configure(encoderFormat(fps, lowLatencyExtras = false), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }
        inputSurface = codec.createInputSurface()
        codec.start()
        listener.onLog("encoder ${codec.name} started")
        encoder = codec
    }

    @Suppress("DEPRECATION") // List<Surface> overload works on all supported API levels.
    private fun createSession(device: CameraDevice, fpsRange: Range<Int>) {
        val surface = inputSurface ?: return
        device.createCaptureSession(listOf(surface), object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(s: CameraCaptureSession) {
                if (stopped) { s.close(); return }
                session = s
                val request = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                    addTarget(surface)
                    set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)
                    set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                }.build()
                try {
                    s.setRepeatingRequest(request, null, handler)
                    listener.onLog("capture started")
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

        /** Prefer 1080p, then 720p, then the largest 16:9 size, then the largest size, capped at 1080p. */
        fun chooseSize(sizes: Array<Size>): Size {
            val capped = sizes.filter { it.width <= 1920 && it.height <= 1080 }
            capped.firstOrNull { it.width == 1920 && it.height == 1080 }?.let { return it }
            capped.firstOrNull { it.width == 1280 && it.height == 720 }?.let { return it }
            capped.filter { it.width * 9 == it.height * 16 }.maxByOrNull { it.width * it.height }?.let { return it }
            return capped.maxByOrNull { it.width * it.height } ?: Size(1280, 720)
        }

        /** Prefer a fixed 30 fps range, else the range with the highest max fps <= 30. */
        fun chooseFpsRange(ranges: Array<Range<Int>>?): Range<Int> {
            if (ranges.isNullOrEmpty()) return Range(30, 30)
            ranges.firstOrNull { it.lower == 30 && it.upper == 30 }?.let { return it }
            return ranges.filter { it.upper <= 30 }.maxWithOrNull(compareBy({ it.upper }, { it.lower }))
                ?: ranges.first()
        }
    }
}
