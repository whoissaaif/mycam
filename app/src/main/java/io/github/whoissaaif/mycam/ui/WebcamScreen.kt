package io.github.whoissaaif.mycam.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.Protocol
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.aero.AeroSegmented
import io.github.whoissaaif.mycam.ui.aero.BadgeKind
import io.github.whoissaaif.mycam.ui.aero.CommandArea
import io.github.whoissaaif.mycam.ui.aero.CommandLink
import io.github.whoissaaif.mycam.ui.aero.GlassHeader
import io.github.whoissaaif.mycam.ui.aero.GlossyBadge
import io.github.whoissaaif.mycam.ui.aero.LivePill
import io.github.whoissaaif.mycam.ui.aero.SectionHeading
import io.github.whoissaaif.mycam.ui.theme.Aero
import io.github.whoissaaif.mycam.ui.theme.MycamTheme

private data class StatusView(val icon: Int, val headline: String, val detail: String, val live: Boolean = false)

@Composable
private fun describe(state: WebcamService.UiState): StatusView = when {
    state.paused -> StatusView(R.drawable.status_paused, stringResource(R.string.status_paused), stringResource(R.string.status_paused_detail))
    state.error != null -> StatusView(R.drawable.status_error, stringResource(R.string.status_error), state.error)
    state.streaming -> StatusView(
        R.drawable.status_streaming, stringResource(R.string.status_streaming),
        stringResource(if (state.facing == Protocol.FACING_FRONT) R.string.camera_front else R.string.camera_back) +
            " · " + state.resolution,
        live = true,
    )
    state.connected -> StatusView(R.drawable.status_ready, stringResource(R.string.status_idle), stringResource(R.string.status_idle_detail))
    else -> StatusView(R.drawable.status_disconnected, stringResource(R.string.status_disconnected), stringResource(R.string.status_disconnected_detail))
}

/** MyCam's main screen, in the Windows 7 Aero style (IMPROVEMENTS.md section 7). */
@Composable
fun WebcamScreen(
    state: WebcamService.UiState,
    onFacing: (Int) -> Unit,
    onPause: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    onCommand: (Int, Int) -> Unit = { _, _ -> },
    onDim: () -> Unit = {},
) {
    val view = describe(state)
    Column(modifier.fillMaxSize().background(Aero.Body)) {
        GlassHeader(stringResource(R.string.app_name), R.drawable.status_ready)

        Column(
            Modifier
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp, vertical = 24.dp),
        ) {
            // Status, like a Win7 task dialog: big icon, blue main instruction, gray detail.
            Row(verticalAlignment = Alignment.CenterVertically) {
                Image(painterResource(view.icon), contentDescription = null, modifier = Modifier.size(72.dp))
                Spacer(Modifier.width(16.dp))
                Column {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(view.headline, style = MaterialTheme.typography.headlineSmall, color = Aero.MainInstruction)
                        if (view.live) {
                            Spacer(Modifier.width(10.dp))
                            LivePill()
                        }
                    }
                    Spacer(Modifier.height(4.dp))
                    Text(view.detail, style = MaterialTheme.typography.bodyMedium, color = Aero.Subtle)
                }
            }
            heatWarning(state.thermal)?.let { (text, _) ->
                Spacer(Modifier.height(12.dp))
                Text(text, style = MaterialTheme.typography.bodyMedium, color = Aero.PausedDark)
            }

            Spacer(Modifier.height(28.dp))
            if (state.paused) {
                CommandLink(
                    title = stringResource(R.string.action_resume_title),
                    description = stringResource(R.string.action_resume_detail),
                    onClick = { onPause(false) },
                ) { GlossyBadge(BadgeKind.Resume, 40.dp) }
            } else {
                CommandLink(
                    title = stringResource(R.string.action_pause_title),
                    description = stringResource(R.string.action_pause_detail),
                    onClick = { onPause(true) },
                ) { GlossyBadge(BadgeKind.Pause, 40.dp) }
            }
            if (state.streaming && !state.paused) {
                CommandLink(
                    title = stringResource(R.string.action_dim_title),
                    description = stringResource(R.string.action_dim_detail),
                    onClick = onDim,
                ) { GlossyBadge(BadgeKind.Dim, 40.dp) }
            }

            Spacer(Modifier.height(32.dp))
            SectionHeading(stringResource(R.string.section_camera))
            Spacer(Modifier.height(12.dp))
            AeroSegmented(
                options = listOf(stringResource(R.string.camera_back), stringResource(R.string.camera_front)),
                selectedIndex = if (state.facing == Protocol.FACING_FRONT) 1 else 0,
                onSelect = { onFacing(if (it == 1) Protocol.FACING_FRONT else Protocol.FACING_BACK) },
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(10.dp))
            Text(stringResource(R.string.camera_hint), style = MaterialTheme.typography.bodyMedium, color = Aero.Subtle)

            Spacer(Modifier.height(32.dp))
            VideoSection(state.camera, state.cameraInfo, onCommand)
            Spacer(Modifier.height(32.dp))
            ControlsSection(state.camera, state.cameraInfo, onCommand)
        }

        CommandArea {
            Text(
                stringResource(if (state.connected) R.string.footer_connected else R.string.footer_disconnected),
                style = MaterialTheme.typography.bodyMedium,
                color = Aero.Subtle,
            )
        }
    }
}

@Preview(showBackground = true, widthDp = 360, heightDp = 720)
@Composable
private fun StreamingPreview() {
    MycamTheme {
        WebcamScreen(WebcamService.UiState(connected = true, streaming = true, resolution = "1920×1080"), {}, {})
    }
}

@Preview(showBackground = true, widthDp = 360, heightDp = 720)
@Composable
private fun PausedPreview() {
    MycamTheme {
        WebcamScreen(WebcamService.UiState(connected = true, paused = true), {}, {})
    }
}
