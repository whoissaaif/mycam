package io.github.whoissaaif.mycam.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.CameraStreamer
import io.github.whoissaaif.mycam.Protocol
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.ui.aero.AeroButton
import io.github.whoissaaif.mycam.ui.aero.AeroCheckbox
import io.github.whoissaaif.mycam.ui.aero.AeroSegmented
import io.github.whoissaaif.mycam.ui.aero.SectionHeading
import io.github.whoissaaif.mycam.ui.theme.Aero
import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt

/** Quality (720p / 1080p / 4K) and frame rate (30 / 60 / 120). Changing them restarts the camera briefly. */
@Composable
fun VideoSection(settings: CameraStreamer.Settings, info: Protocol.CameraInfo?, onCommand: (Int, Int) -> Unit) {
    val has4K = info == null || info.flags and Protocol.CAM_HAS_4K != 0
    // Frame rates that work at the selected quality on this phone (until the camera has reported, only 30).
    val mask = if (info != null && info.width > 0) info.fpsMask(settings.quality) else Protocol.FPS_30
    val has60 = mask and Protocol.FPS_60 != 0
    val has120 = mask and Protocol.FPS_120 != 0
    SectionHeading(stringResource(R.string.section_video))
    Spacer(Modifier.height(12.dp))
    AeroSegmented(
        options = listOf("720p", "1080p", "4K"),
        selectedIndex = settings.quality,
        onSelect = { onCommand(Protocol.CMD_SET_QUALITY, it) },
        enabled = { it != Protocol.QUALITY_4K || has4K },
        modifier = Modifier.fillMaxWidth(),
    )
    Spacer(Modifier.height(10.dp))
    val rates = listOf(30, 60, 120)
    val available = { i: Int -> i == 0 || (i == 1 && has60) || (i == 2 && has120) }
    AeroSegmented(
        options = rates.map { "$it fps" },
        // Show what this quality will actually run at: the chosen rate, or the best one below it that works
        // here (the choice is kept, so going back to a quality that supports it restores it).
        selectedIndex = rates.indices.last { it == 0 || (rates[it] <= settings.fps && available(it)) },
        onSelect = { onCommand(Protocol.CMD_SET_FPS, rates[it]) },
        enabled = available,
        modifier = Modifier.fillMaxWidth(),
    )
    Spacer(Modifier.height(6.dp))
    Text(
        stringResource(if (has60 || has120) R.string.video_fps_hint else R.string.video_fps_unsupported),
        style = MaterialTheme.typography.bodyMedium, color = Aero.Subtle,
    )
    if (info != null && info.width > 0) {
        Text(
            stringResource(R.string.video_actual, "${info.width}×${info.height}", info.actualFps),
            style = MaterialTheme.typography.bodyMedium, color = Aero.Subtle,
        )
    }
}

/** Zoom, exposure, torch and focus, applied live. Ranges come from the camera once it has started. */
@Composable
fun ControlsSection(settings: CameraStreamer.Settings, info: Protocol.CameraInfo?, onCommand: (Int, Int) -> Unit) {
    SectionHeading(stringResource(R.string.section_controls))
    Spacer(Modifier.height(12.dp))
    if (info == null || info.width == 0) {
        Text(stringResource(R.string.controls_unknown), style = MaterialTheme.typography.bodyMedium, color = Aero.Subtle)
        return
    }
    val zoomMin = info.zoomMinX100 / 100f
    val zoomMax = info.zoomMaxX100 / 100f
    val zoom = info.zoomX100 / 100f
    fun setZoom(z: Float) = onCommand(Protocol.CMD_SET_ZOOM, (z.coerceIn(zoomMin, zoomMax) * 10).roundToInt().coerceIn(1, 255))

    // Zoom: lens-style presets (the ultrawide shows up as a preset below 1x where the phone supports it).
    val presets = listOfNotNull(zoomMin.takeIf { it < 0.95f }, 1f, 2f.takeIf { zoomMax >= 2f }, 5f.takeIf { zoomMax >= 5f })
    Text(stringResource(R.string.control_zoom, format(zoom) + "×"), style = MaterialTheme.typography.bodyLarge)
    Spacer(Modifier.height(8.dp))
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        AeroButton("−", onClick = { setZoom(zoom / 1.25f) }, enabled = zoom > zoomMin + 0.01f, modifier = Modifier.width(52.dp))
        AeroSegmented(
            options = presets.map { format(it) + "×" },
            selectedIndex = presets.indices.minByOrNull { abs(presets[it] - zoom) }?.takeIf { abs(presets[it] - zoom) < 0.05f } ?: -1,
            onSelect = { setZoom(presets[it]) },
            modifier = Modifier.weight(1f),
        )
        AeroButton("+", onClick = { setZoom(zoom * 1.25f) }, enabled = zoom < zoomMax - 0.01f, modifier = Modifier.width(52.dp))
    }

    // Exposure compensation.
    if (info.evMax > info.evMin) {
        Spacer(Modifier.height(16.dp))
        val evValue = info.ev * info.evStepX100 / 100f
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(stringResource(R.string.control_exposure), style = MaterialTheme.typography.bodyLarge, modifier = Modifier.weight(1f))
            AeroButton("−", onClick = { onCommand(Protocol.CMD_SET_EXPOSURE, (info.ev - 1) and 0xFF) },
                enabled = info.ev > info.evMin, modifier = Modifier.width(52.dp))
            Text(
                (if (evValue > 0) "+" else "") + format(evValue) + " EV",
                style = MaterialTheme.typography.bodyLarge, textAlign = TextAlign.Center, modifier = Modifier.width(80.dp),
            )
            AeroButton("+", onClick = { onCommand(Protocol.CMD_SET_EXPOSURE, (info.ev + 1) and 0xFF) },
                enabled = info.ev < info.evMax, modifier = Modifier.width(52.dp))
        }
    }

    // Focus.
    if (info.flags and Protocol.CAM_HAS_AUTOFOCUS != 0) {
        Spacer(Modifier.height(16.dp))
        Text(stringResource(R.string.control_focus), style = MaterialTheme.typography.bodyLarge)
        Spacer(Modifier.height(8.dp))
        AeroSegmented(
            options = listOf(stringResource(R.string.focus_auto), stringResource(R.string.focus_lock)),
            selectedIndex = if (info.flags and Protocol.CAM_FOCUS_LOCKED != 0) 1 else 0,
            onSelect = { onCommand(Protocol.CMD_SET_FOCUS, it) },
            modifier = Modifier.fillMaxWidth(),
        )
    }

    // Torch (back camera with a flash).
    if (info.flags and Protocol.CAM_TORCH_AVAILABLE != 0) {
        Spacer(Modifier.height(8.dp))
        AeroCheckbox(
            stringResource(R.string.control_torch), checked = info.flags and Protocol.CAM_TORCH_ON != 0,
            onCheckedChange = { onCommand(Protocol.CMD_SET_TORCH, if (it) 1 else 0) },
        )
    }

    // Auto: one tap back to the defaults (1x zoom, normal brightness, auto focus, torch off).
    val atDefaults = info.zoomX100 == 100.coerceIn(info.zoomMinX100, info.zoomMaxX100) && info.ev == 0 &&
        info.flags and (Protocol.CAM_TORCH_ON or Protocol.CAM_FOCUS_LOCKED) == 0
    Spacer(Modifier.height(16.dp))
    AeroButton(
        stringResource(R.string.controls_auto),
        onClick = {
            onCommand(Protocol.CMD_SET_ZOOM, 10)
            onCommand(Protocol.CMD_SET_EXPOSURE, 0)
            onCommand(Protocol.CMD_SET_FOCUS, 0)
            onCommand(Protocol.CMD_SET_TORCH, 0)
        },
        enabled = !atDefaults,
        modifier = Modifier.fillMaxWidth(),
    )
}

private fun format(v: Float): String =
    if (abs(v - v.roundToInt()) < 0.05f) v.roundToInt().toString() else String.format(Locale.US, "%.1f", v)
