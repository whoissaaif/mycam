package io.github.whoissaaif.mycam.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
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
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.CameraStreamer
import io.github.whoissaaif.mycam.Protocol
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.ui.theme.Xp
import io.github.whoissaaif.mycam.ui.xp.XpButton
import io.github.whoissaaif.mycam.ui.xp.XpCheckbox
import io.github.whoissaaif.mycam.ui.xp.XpRadioGroup
import io.github.whoissaaif.mycam.ui.xp.XpToggleRow
import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt

private val RATES = listOf(30, 60, 120)

/**
 * Quality (720p / 1080p / 4K) and frame rate (30 / 60 / 120) as XP radios, with a per-state reason for the
 * greyed rates. What works comes from the phone's report (CameraStreamer.modeFor decides it on the phone).
 * Changing either restarts the camera briefly.
 */
@Composable
fun VideoSettings(
    settings: CameraStreamer.Settings,
    info: Protocol.CameraInfo?,
    streaming: Boolean,
    onCommand: (Int, Int) -> Unit,
) {
    val reported = info != null && info.width > 0
    val has4K = info == null || info.flags and Protocol.CAM_HAS_4K != 0
    val qualityAvailable = { q: Int -> q != Protocol.QUALITY_4K || has4K }
    // Frame rates that work at the selected quality on this phone (until the camera has reported, only 30).
    val mask = if (reported) info.fpsMask(settings.quality) else Protocol.FPS_30
    val available = { i: Int ->
        i == 0 || (i == 1 && mask and Protocol.FPS_60 != 0) || (i == 2 && mask and Protocol.FPS_120 != 0)
    }

    Label(stringResource(R.string.video_quality))
    XpRadioGroup(
        options = listOf("720p", "1080p", "4K"),
        selectedIndex = settings.quality,
        onSelect = { onCommand(Protocol.CMD_SET_QUALITY, it) },
        enabled = qualityAvailable,
    )
    Spacer(Modifier.height(8.dp))
    Label(stringResource(R.string.video_fps))
    XpRadioGroup(
        options = RATES.map { "$it fps" },
        // Show what this quality will actually run at: the chosen rate, or the best one below it that works
        // here (the choice is kept, so going back to a quality that supports it restores it).
        selectedIndex = RATES.indices.last { it == 0 || (RATES[it] <= settings.fps && available(it)) },
        onSelect = { onCommand(Protocol.CMD_SET_FPS, RATES[it]) },
        enabled = available,
    )
    Spacer(Modifier.height(4.dp))
    FpsReason.compute(info, settings.quality, qualityAvailable).forEach { reason ->
        Text(fpsReasonText(reason), style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle)
    }
    if (reported && streaming) {
        Spacer(Modifier.height(4.dp))
        Text(
            stringResource(R.string.video_actual, "${info.width}×${info.height}", info.actualFps),
            style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle,
        )
    }
}

@Composable
private fun fpsReasonText(r: FpsReason): String = when (r) {
    FpsReason.Unknown -> stringResource(R.string.video_fps_unknown)
    FpsReason.Only30 -> stringResource(R.string.video_fps_only_30)
    is FpsReason.NeedsOrLower ->
        if (r.quality == Protocol.QUALITY_720P) stringResource(R.string.video_fps_needs, r.fps, FpsReason.qualityName(r.quality))
        else stringResource(R.string.video_fps_needs_or_lower, r.fps, FpsReason.qualityName(r.quality))
    is FpsReason.Needs -> stringResource(R.string.video_fps_needs, r.fps, r.qualities.joinToString(" / ") { FpsReason.qualityName(it) })
    is FpsReason.UpTo -> stringResource(R.string.video_fps_up_to, r.maxFps)
}

@Composable
private fun Label(text: String) {
    Text(text, style = MaterialTheme.typography.titleMedium, color = Xp.Text, modifier = Modifier.semantics { heading() })
}

/**
 * Zoom, brightness, focus and torch, applied live, plus Reset. Only what this camera reported is shown;
 * the caller shows one grey line instead until the camera has reported.
 */
@Composable
fun QuickControls(info: Protocol.CameraInfo, onCommand: (Int, Int) -> Unit) {
    val zoomMin = info.zoomMinX100 / 100f
    val zoomMax = info.zoomMaxX100 / 100f
    val zoom = info.zoomX100 / 100f
    fun setZoom(z: Float) = onCommand(Protocol.CMD_SET_ZOOM, (z.coerceIn(zoomMin, zoomMax) * 10).roundToInt().coerceIn(1, 255))

    Column(Modifier.fillMaxWidth()) {
        // Zoom: lens-style presets (the ultrawide shows up as a preset below 1x where the phone supports it).
        if (zoomMax > zoomMin) {
            val presets = listOfNotNull(zoomMin.takeIf { it < 0.95f }, 1f, 2f.takeIf { zoomMax >= 2f }, 5f.takeIf { zoomMax >= 5f })
            Text(stringResource(R.string.control_zoom_value, format(zoom) + "×"), style = MaterialTheme.typography.bodyMedium, color = Xp.Text)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp), verticalAlignment = Alignment.CenterVertically) {
                StepButton("−", stringResource(R.string.zoom_out), enabled = zoom > zoomMin + 0.01f) { setZoom(zoom / 1.25f) }
                XpToggleRow(
                    options = presets.map { format(it) + "×" },
                    selectedIndex = presets.indices.minByOrNull { abs(presets[it] - zoom) }?.takeIf { abs(presets[it] - zoom) < 0.05f } ?: -1,
                    onSelect = { setZoom(presets[it]) },
                    modifier = Modifier.weight(1f),
                )
                StepButton("+", stringResource(R.string.zoom_in), enabled = zoom < zoomMax - 0.01f) { setZoom(zoom * 1.25f) }
            }
        }

        // Brightness (exposure compensation).
        if (info.evMax > info.evMin) {
            Spacer(Modifier.height(8.dp))
            val evValue = info.ev * info.evStepX100 / 100f
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.control_brightness), style = MaterialTheme.typography.bodyMedium, color = Xp.Text, modifier = Modifier.weight(1f))
                StepButton("−", stringResource(R.string.brightness_down), enabled = info.ev > info.evMin) {
                    onCommand(Protocol.CMD_SET_EXPOSURE, (info.ev - 1) and 0xFF)
                }
                Text(
                    (if (evValue > 0) "+" else "") + format(evValue) + " EV",
                    style = MaterialTheme.typography.bodyMedium, color = Xp.Text, textAlign = TextAlign.Center,
                    modifier = Modifier.width(76.dp),
                )
                StepButton("+", stringResource(R.string.brightness_up), enabled = info.ev < info.evMax) {
                    onCommand(Protocol.CMD_SET_EXPOSURE, (info.ev + 1) and 0xFF)
                }
            }
        }

        // Focus.
        if (info.flags and Protocol.CAM_HAS_AUTOFOCUS != 0) {
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.control_focus), style = MaterialTheme.typography.bodyMedium, color = Xp.Text, modifier = Modifier.width(96.dp))
                XpRadioGroup(
                    options = listOf(stringResource(R.string.focus_auto), stringResource(R.string.focus_lock)),
                    selectedIndex = if (info.flags and Protocol.CAM_FOCUS_LOCKED != 0) 1 else 0,
                    onSelect = { onCommand(Protocol.CMD_SET_FOCUS, it) },
                )
            }
        }

        // Torch (back camera with a flash) and Reset: one tap back to 1x, normal brightness, auto focus, torch off.
        val atDefaults = info.zoomX100 == 100.coerceIn(info.zoomMinX100, info.zoomMaxX100) && info.ev == 0 &&
            info.flags and (Protocol.CAM_TORCH_ON or Protocol.CAM_FOCUS_LOCKED) == 0
        Spacer(Modifier.height(4.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (info.flags and Protocol.CAM_TORCH_AVAILABLE != 0) {
                XpCheckbox(
                    stringResource(R.string.control_torch), checked = info.flags and Protocol.CAM_TORCH_ON != 0,
                    onCheckedChange = { onCommand(Protocol.CMD_SET_TORCH, if (it) 1 else 0) },
                    modifier = Modifier.weight(1f),
                )
            } else {
                Spacer(Modifier.weight(1f))
            }
            XpButton(
                stringResource(R.string.controls_reset),
                onClick = {
                    onCommand(Protocol.CMD_SET_ZOOM, 10)
                    onCommand(Protocol.CMD_SET_EXPOSURE, 0)
                    onCommand(Protocol.CMD_SET_FOCUS, 0)
                    onCommand(Protocol.CMD_SET_TORCH, 0)
                },
                enabled = !atDefaults,
                contentDescription = stringResource(R.string.controls_reset_description),
            )
        }
    }
}

/** The square − / + button with a spoken label. */
@Composable
private fun StepButton(symbol: String, description: String, enabled: Boolean, onClick: () -> Unit) {
    XpButton(symbol, onClick = onClick, enabled = enabled, contentDescription = description, modifier = Modifier.width(48.dp))
}

private fun format(v: Float): String =
    if (abs(v - v.roundToInt()) < 0.05f) v.roundToInt().toString() else String.format(Locale.US, "%.1f", v)
