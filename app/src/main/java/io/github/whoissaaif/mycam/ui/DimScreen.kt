package io.github.whoissaaif.mycam.ui

import android.os.PowerManager
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.Protocol
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.aero.LivePill
import io.github.whoissaaif.mycam.ui.theme.Aero
import io.github.whoissaaif.mycam.ui.theme.MycamTheme
import kotlinx.coroutines.delay

/**
 * The screen shown while streaming once the phone has been left alone (IMPROVEMENTS.md 4.1): nearly black,
 * so it runs cooler and shows nothing private. The activity wakes it on any touch.
 */
@Composable
fun DimScreen(state: WebcamService.UiState, modifier: Modifier = Modifier) {
    // Drift the content a little every minute so nothing stays lit in the same place (OLED burn-in).
    var step by remember { mutableIntStateOf(0) }
    LaunchedEffect(Unit) {
        while (true) {
            delay(60_000)
            step++
        }
    }
    val dx = listOf(0, 14, 6, -12, -4)[step % 5]
    val dy = listOf(0, -20, 18, 8, -14)[step % 5]

    Box(
        modifier
            .fillMaxSize()
            .background(Brush.verticalGradient(listOf(Aero.DimTop, Aero.DimBottom)))
            .drawBehind {
                drawCircle(
                    Brush.radialGradient(
                        listOf(Aero.DimGlow.copy(alpha = 0.55f), Aero.DimGlow.copy(alpha = 0f)),
                        center = Offset(size.width / 2, size.height * 0.42f), radius = size.width * 0.7f,
                    ),
                    radius = size.width * 0.7f, center = Offset(size.width / 2, size.height * 0.42f),
                )
            },
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.offset(dx.dp, dy.dp).padding(32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Image(
                painterResource(R.drawable.status_streaming), contentDescription = null,
                modifier = Modifier.size(64.dp).alpha(0.5f),
            )
            Spacer(Modifier.height(18.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(stringResource(R.string.dim_title), style = MaterialTheme.typography.titleMedium, color = Aero.DimText)
                Spacer(Modifier.width(10.dp))
                LivePill(Modifier.alpha(0.7f))
            }
            Spacer(Modifier.height(6.dp))
            Text(
                stringResource(if (state.facing == Protocol.FACING_FRONT) R.string.camera_front else R.string.camera_back) +
                    " · " + state.resolution,
                style = MaterialTheme.typography.bodyMedium, color = Aero.DimSubtle,
            )
            heatWarning(state.thermal)?.let { (text, color) ->
                Spacer(Modifier.height(24.dp))
                Text(text, style = MaterialTheme.typography.bodyMedium, color = color, textAlign = TextAlign.Center)
            }
            Spacer(Modifier.height(48.dp))
            Text(stringResource(R.string.dim_hint), style = MaterialTheme.typography.bodySmall, color = Aero.DimSubtle)
        }
    }
}

/** Warning text and colour for a PowerManager thermal status, or null below MODERATE. */
@Composable
fun heatWarning(thermal: Int) = when {
    thermal >= PowerManager.THERMAL_STATUS_SEVERE -> stringResource(R.string.heat_hot) to Aero.HeatHot
    thermal >= PowerManager.THERMAL_STATUS_MODERATE -> stringResource(R.string.heat_warm) to Aero.HeatWarm
    else -> null
}

@Preview(widthDp = 360, heightDp = 720)
@Composable
private fun DimPreview() {
    MycamTheme {
        DimScreen(WebcamService.UiState(connected = true, streaming = true, resolution = "1920×1080", thermal = 2))
    }
}
