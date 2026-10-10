package io.github.whoissaaif.mycam.ui

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.BackHandler
import androidx.compose.animation.Crossfade
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.scale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalUriHandler
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.PairedPc
import io.github.whoissaaif.mycam.Protocol
import io.github.whoissaaif.mycam.R
import io.github.whoissaaif.mycam.WebcamService
import io.github.whoissaaif.mycam.ui.theme.LocalReducedMotion
import io.github.whoissaaif.mycam.ui.theme.MycamTheme
import io.github.whoissaaif.mycam.ui.theme.Xp
import io.github.whoissaaif.mycam.ui.xp.BadgeKind
import io.github.whoissaaif.mycam.ui.xp.BottomBar
import io.github.whoissaaif.mycam.ui.xp.BottomBarItem
import io.github.whoissaaif.mycam.ui.xp.GlossyBadge
import io.github.whoissaaif.mycam.ui.xp.GroupKind
import io.github.whoissaaif.mycam.ui.xp.HeroCommandButton
import io.github.whoissaaif.mycam.ui.xp.LivePill
import io.github.whoissaaif.mycam.ui.xp.TaskGroup
import io.github.whoissaaif.mycam.ui.xp.TitleBarHeader
import io.github.whoissaaif.mycam.ui.xp.WidthAware
import io.github.whoissaaif.mycam.ui.xp.XpButton
import io.github.whoissaaif.mycam.ui.xp.XpCheckbox
import io.github.whoissaaif.mycam.ui.xp.XpLink
import io.github.whoissaaif.mycam.ui.xp.XpProgressBar
import io.github.whoissaaif.mycam.ui.xp.XpRadioGroup
import io.github.whoissaaif.mycam.ui.xp.XpTabs
import io.github.whoissaaif.mycam.ui.xp.motionMs
import io.github.whoissaaif.mycam.ui.xp.popSpring
import io.github.whoissaaif.mycam.ui.xp.taskPane
import kotlinx.coroutines.delay

/** Where "Get it" and About point: MyCam for Windows. */
const val RELEASES_URL = "https://github.com/whoissaaif/mycam/releases"

/** What the main screen can ask the activity to do. Defaults are no-ops (previews). */
data class ScreenActions(
    val onFacing: (Int) -> Unit = {},
    val onPause: (Boolean) -> Unit = {},
    val onCommand: (Int, Int) -> Unit = { _, _ -> },
    val onDim: () -> Unit = {},
    val onWireless: (Boolean) -> Unit = {},
    val onAnswerPc: (Boolean) -> Unit = {},
    val onForgetPc: (String) -> Unit = {},
    val onForgetAllPcs: () -> Unit = {},
    val onAutoDim: (Boolean) -> Unit = {},
    val onDismissTimedOut: () -> Unit = {},
)

private data class StatusView(val icon: Int, val headline: String, val detail: String, val live: Boolean = false)

@Composable
private fun describe(state: WebcamService.UiState): StatusView = when {
    state.paused -> StatusView(R.drawable.status_paused, stringResource(R.string.status_paused), stringResource(R.string.status_paused_detail))
    state.error != null -> StatusView(R.drawable.status_error, stringResource(R.string.status_error), state.error)
    state.streaming -> StatusView(
        R.drawable.status_streaming, stringResource(R.string.status_streaming),
        stringResource(if (state.facing == Protocol.FACING_FRONT) R.string.camera_front else R.string.camera_back) +
            " · " + state.resolution + if (state.wireless) " · Wi-Fi" else " · USB",
        live = true,
    )
    state.connected && state.wireless -> StatusView(
        R.drawable.status_ready, stringResource(R.string.status_idle),
        stringResource(R.string.wireless_connected, state.wirelessPc ?: ""),
    )
    state.connected -> StatusView(R.drawable.status_ready, stringResource(R.string.status_idle), stringResource(R.string.status_idle_detail))
    state.wirelessOn -> StatusView(
        R.drawable.status_disconnected, stringResource(R.string.status_wireless_waiting),
        stringResource(R.string.status_wireless_waiting_detail),
    )
    else -> StatusView(R.drawable.status_disconnected, stringResource(R.string.status_disconnected), stringResource(R.string.status_disconnected_detail))
}

/** A quality / fps / camera change on its way: the marquee runs until the phone reports [done] (or 4 s). */
private class Applying(val startInfo: Protocol.CameraInfo?, val done: (WebcamService.UiState) -> Boolean)

private const val TAB_NOW = 0
private const val TAB_SETTINGS = 1

/** MyCam's main screen in the Windows XP Luna style (redesign.md section 5): Now and Settings tabs. */
@Composable
fun WebcamScreen(
    state: WebcamService.UiState,
    modifier: Modifier = Modifier,
    actions: ScreenActions = ScreenActions(),
    autoDim: Boolean = true,
    initialTab: Int = TAB_NOW,
) {
    var tab by rememberSaveable { mutableStateOf(initialTab) }
    var wirelessOpen by rememberSaveable { mutableStateOf(true) }
    var showLicences by remember { mutableStateOf(false) }

    // Applying feedback (A14): only while the camera runs, since that's when a change restarts it.
    var applying by remember { mutableStateOf<Applying?>(null) }
    LaunchedEffect(applying) {
        if (applying != null) {
            delay(4_000)
            applying = null
        }
    }
    LaunchedEffect(state, applying) {
        val a = applying ?: return@LaunchedEffect
        if (state.cameraInfo !== a.startInfo && a.done(state)) applying = null
    }
    val live = state.streaming && !state.paused
    // The LIVE pill's gloss sweep plays once per stream, not again on every tab switch.
    var liveSwept by rememberSaveable { mutableStateOf(false) }
    LaunchedEffect(state.streaming) { if (!state.streaming) liveSwept = false }
    val onCommand: (Int, Int) -> Unit = { cmd, arg ->
        if (live && cmd == Protocol.CMD_SET_QUALITY && arg != state.camera.quality) {
            applying = Applying(state.cameraInfo) { it.cameraInfo?.quality == arg }
        } else if (live && cmd == Protocol.CMD_SET_FPS && arg != state.camera.fps) {
            applying = Applying(state.cameraInfo) { it.cameraInfo?.fps == arg }
        }
        actions.onCommand(cmd, arg)
    }
    val onFacing: (Int) -> Unit = { f ->
        if (live && f != state.facing) applying = Applying(state.cameraInfo) { it.facing == f }
        actions.onFacing(f)
    }

    val pairing = state.pendingPc != null || state.pairingTimedOut != null
    val modal = pairing || showLicences
    val blur by animateDpAsState(if (modal) 16.dp else 0.dp, tween(motionMs(220)), label = "backdropBlur")

    BackHandler(enabled = tab == TAB_SETTINGS && !modal) { tab = TAB_NOW }

    Box(modifier.fillMaxSize()) {
        Column(
            Modifier
                .fillMaxSize()
                .background(Xp.Surface)
                .then(if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && blur > 0.dp) Modifier.blur(blur) else Modifier)
                .then(if (modal) Modifier.clearAndSetSemantics { } else Modifier),
        ) {
            TitleBarHeader(stringResource(R.string.app_name), R.drawable.status_ready)
            XpTabs(
                titles = listOf(stringResource(R.string.tab_now), stringResource(R.string.tab_settings)),
                selected = tab, onSelect = { tab = it },
            )
            Crossfade(tab, Modifier.weight(1f), animationSpec = tween(motionMs(150)), label = "tab") { t ->
                if (t == TAB_NOW) {
                    NowTab(
                        state, applying != null, actions, onFacing, onCommand, onGetIt = { tab = TAB_SETTINGS },
                        sweepLive = !liveSwept, onLiveSwept = { liveSwept = true },
                    )
                } else {
                    SettingsTab(
                        state, applying != null, actions, onCommand, autoDim,
                        wirelessOpen = wirelessOpen, onWirelessOpen = { wirelessOpen = it },
                        onLicences = { showLicences = true },
                    )
                }
            }
            BottomBar {
                if (live) {
                    BottomBarItem(stringResource(R.string.action_dim), onClick = actions.onDim, icon = { GlossyBadge(BadgeKind.Dim, 24.dp) })
                }
                Spacer(Modifier.weight(1f))
                BottomBarItem(
                    stringResource(
                        when {
                            state.connected && state.wireless -> R.string.wifi_status_connected
                            state.wirelessOn -> R.string.wifi_status_on
                            else -> R.string.wifi_status_off
                        }
                    ),
                    onClick = { tab = TAB_SETTINGS; wirelessOpen = true },
                )
            }
        }

        PairingDialog(
            pcName = state.pendingPc ?: state.pairingTimedOut,
            code = state.pendingCode,
            deadline = state.pendingDeadline,
            timedOut = state.pendingPc == null && state.pairingTimedOut != null,
            onAnswer = actions.onAnswerPc,
            onDismissTimedOut = actions.onDismissTimedOut,
        )
        LicenceDialog(visible = showLicences && !pairing, onClose = { showLicences = false })
    }
}

// --- Now ---------------------------------------------------------------------------------------------

@Composable
private fun NowTab(
    state: WebcamService.UiState,
    applying: Boolean,
    actions: ScreenActions,
    onFacing: (Int) -> Unit,
    onCommand: (Int, Int) -> Unit,
    onGetIt: () -> Unit,
    sweepLive: Boolean,
    onLiveSwept: () -> Unit,
) {
    var cameraOpen by rememberSaveable { mutableStateOf(true) }
    WidthAware { wide ->
        val scroll = rememberScrollState()
        if (wide) {
            Row(
                Modifier.fillMaxSize().taskPane().verticalScroll(scroll).padding(12.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                HeroGroup(state, applying, actions, onGetIt, sweepLive, onLiveSwept, Modifier.weight(0.4f))
                CameraGroup(state, onFacing, onCommand, cameraOpen, { cameraOpen = !cameraOpen }, Modifier.weight(0.6f))
            }
        } else {
            Column(Modifier.fillMaxSize().taskPane().verticalScroll(scroll).padding(12.dp)) {
                HeroGroup(state, applying, actions, onGetIt, sweepLive, onLiveSwept)
                Spacer(Modifier.height(12.dp))
                CameraGroup(state, onFacing, onCommand, cameraOpen, { cameraOpen = !cameraOpen })
            }
        }
    }
}

/** The "Now" hero group: status illustration, headline, detail and the Pause / Resume command. */
@Composable
private fun HeroGroup(
    state: WebcamService.UiState,
    applying: Boolean,
    actions: ScreenActions,
    onGetIt: () -> Unit,
    sweepLive: Boolean,
    onLiveSwept: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val view = describe(state)
    val uri = LocalUriHandler.current
    TaskGroup(
        title = view.headline,
        modifier = modifier,
        kind = if (state.paused) GroupKind.Paused else GroupKind.Hero,
        bodyColor = Xp.Card,
        trailing = if (view.live) ({ LivePill(sweep = sweepLive, onSwept = onLiveSwept) }) else null,
        belowHeader = if (applying) ({ XpProgressBar(null, Modifier.padding(start = 12.dp, end = 12.dp, top = 10.dp)) }) else null,
    ) {
        Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
            StatusIllustration(view.icon)
            Spacer(Modifier.height(8.dp))
            Text(view.detail, style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle, textAlign = TextAlign.Center)
            if (!state.connected && !state.wirelessOn) {
                XpLink(stringResource(R.string.get_it), onClick = {
                    try { uri.openUri(RELEASES_URL) } catch (_: Exception) { onGetIt() }
                })
            }
        }
        heatWarning(state.thermal)?.let { (text, _) ->
            Spacer(Modifier.height(8.dp))
            Text(text, style = MaterialTheme.typography.bodyMedium, color = Xp.WarningText)
        }
        Spacer(Modifier.height(12.dp))
        if (state.paused) {
            HeroCommandButton(
                stringResource(R.string.action_resume_title), stringResource(R.string.action_resume_detail),
                BadgeKind.Resume, onClick = { actions.onPause(false) }, go = true,
            )
        } else {
            HeroCommandButton(
                stringResource(R.string.action_pause_title), stringResource(R.string.action_pause_detail),
                BadgeKind.Pause, onClick = { actions.onPause(true) },
            )
        }
    }
}

/** The big status picture (the logo with its state badge). Crossfades between states with a small pop. */
@Composable
private fun StatusIllustration(icon: Int) {
    val reduced = LocalReducedMotion.current
    Crossfade(icon, animationSpec = tween(motionMs(200)), label = "status") { res ->
        val pop = remember { Animatable(if (reduced) 1f else 0.85f) }
        LaunchedEffect(Unit) { if (!reduced) pop.animateTo(1f, popSpring()) }
        Image(painterResource(res), contentDescription = null, modifier = Modifier.size(96.dp).scale(pop.value))
    }
}

@Composable
private fun CameraGroup(
    state: WebcamService.UiState,
    onFacing: (Int) -> Unit,
    onCommand: (Int, Int) -> Unit,
    expanded: Boolean,
    onToggle: () -> Unit,
    modifier: Modifier = Modifier,
) {
    TaskGroup(stringResource(R.string.section_camera), modifier, expanded = expanded, onToggle = onToggle) {
        XpRadioGroup(
            options = listOf(stringResource(R.string.camera_back), stringResource(R.string.camera_front)),
            selectedIndex = if (state.facing == Protocol.FACING_FRONT) 1 else 0,
            onSelect = { onFacing(if (it == 1) Protocol.FACING_FRONT else Protocol.FACING_BACK) },
        )
        val info = state.cameraInfo
        if (state.streaming && info != null && info.width > 0) {
            Spacer(Modifier.height(4.dp))
            QuickControls(info, onCommand)
        } else {
            Text(stringResource(R.string.controls_unknown), style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle)
        }
    }
}

// --- Settings ----------------------------------------------------------------------------------------

@Composable
private fun SettingsTab(
    state: WebcamService.UiState,
    applying: Boolean,
    actions: ScreenActions,
    onCommand: (Int, Int) -> Unit,
    autoDim: Boolean,
    wirelessOpen: Boolean,
    onWirelessOpen: (Boolean) -> Unit,
    onLicences: () -> Unit,
) {
    var videoOpen by rememberSaveable { mutableStateOf(true) }
    var pairedOpen by rememberSaveable { mutableStateOf(true) }
    var phoneOpen by rememberSaveable { mutableStateOf(true) }
    var aboutOpen by rememberSaveable { mutableStateOf(false) }

    val video: @Composable (Modifier) -> Unit = { m ->
        TaskGroup(
            stringResource(R.string.section_video), m, expanded = videoOpen, onToggle = { videoOpen = !videoOpen },
            belowHeader = if (applying) ({ XpProgressBar(null, Modifier.padding(start = 12.dp, end = 12.dp, top = 10.dp)) }) else null,
        ) { VideoSettings(state.camera, state.cameraInfo, state.streaming, onCommand) }
    }
    val wireless: @Composable (Modifier) -> Unit = { m ->
        TaskGroup(stringResource(R.string.section_wireless), m, expanded = wirelessOpen, onToggle = { onWirelessOpen(!wirelessOpen) }) {
            XpCheckbox(stringResource(R.string.wireless_toggle), checked = state.wirelessOn, onCheckedChange = actions.onWireless)
            Text(
                when {
                    state.connected && state.wireless -> stringResource(R.string.wireless_connected, state.wirelessPc ?: "")
                    !state.wirelessOn -> stringResource(R.string.wireless_hint_off)
                    state.wirelessAddress == null -> stringResource(R.string.wireless_hint_no_wifi)
                    else -> stringResource(R.string.wireless_hint_on, state.wirelessAddress)
                },
                style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle,
            )
        }
    }
    val paired: @Composable (Modifier) -> Unit = { m ->
        TaskGroup(stringResource(R.string.section_paired), m, expanded = pairedOpen, onToggle = { pairedOpen = !pairedOpen }) {
            PairedPcsList(state.pairedPcs, actions.onForgetPc, actions.onForgetAllPcs)
        }
    }
    val phone: @Composable (Modifier) -> Unit = { m ->
        TaskGroup(stringResource(R.string.section_phone), m, expanded = phoneOpen, onToggle = { phoneOpen = !phoneOpen }) {
            XpCheckbox(stringResource(R.string.auto_dim_toggle), checked = autoDim, onCheckedChange = actions.onAutoDim)
            Text(stringResource(R.string.auto_dim_hint), style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle)
        }
    }
    val about: @Composable (Modifier) -> Unit = { m ->
        TaskGroup(stringResource(R.string.section_about), m, expanded = aboutOpen, onToggle = { aboutOpen = !aboutOpen }) {
            val uri = LocalUriHandler.current
            XpLink(stringResource(R.string.about_get_windows), onClick = { try { uri.openUri(RELEASES_URL) } catch (_: Exception) {} })
            Text(stringResource(R.string.about_version, appVersion(LocalContext.current)), style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle)
            XpLink(stringResource(R.string.about_licences), onClick = onLicences)
        }
    }

    WidthAware { wide ->
        val scroll = rememberScrollState()
        if (wide) {
            Row(
                Modifier.fillMaxSize().background(Xp.Surface).verticalScroll(scroll).padding(12.dp),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    video(Modifier)
                    wireless(Modifier)
                }
                Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    paired(Modifier)
                    phone(Modifier)
                    about(Modifier)
                }
            }
        } else {
            Column(
                Modifier.fillMaxSize().background(Xp.Surface).verticalScroll(scroll).padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                video(Modifier)
                wireless(Modifier)
                paired(Modifier)
                phone(Modifier)
                about(Modifier)
            }
        }
    }
}

/** One row per paired PC with its own Forget (A11), plus Forget all when there are several. */
@Composable
private fun PairedPcsList(pcs: List<PairedPc>, onForget: (String) -> Unit, onForgetAll: () -> Unit) {
    if (pcs.isEmpty()) {
        Text(stringResource(R.string.paired_none), style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle)
        return
    }
    pcs.forEach { pc ->
        Row(Modifier.fillMaxWidth().heightIn(min = 52.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(pc.name, style = MaterialTheme.typography.bodyMedium, color = Xp.Text, modifier = Modifier.weight(1f))
            XpButton(
                stringResource(R.string.paired_forget), onClick = { onForget(pc.id) },
                contentDescription = stringResource(R.string.paired_forget_description, pc.name),
            )
        }
    }
    Text(stringResource(R.string.paired_hint), style = MaterialTheme.typography.bodyMedium, color = Xp.Subtle)
    if (pcs.size > 1) {
        Spacer(Modifier.height(4.dp))
        XpButton(stringResource(R.string.paired_forget_all), onClick = onForgetAll, modifier = Modifier.widthIn(min = 120.dp))
    }
}

private fun appVersion(context: Context): String = try {
    val info = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        context.packageManager.getPackageInfo(context.packageName, PackageManager.PackageInfoFlags.of(0))
    } else {
        @Suppress("DEPRECATION")
        context.packageManager.getPackageInfo(context.packageName, 0)
    }
    info.versionName ?: "?"
} catch (_: Exception) {
    "?"
}

// --- Previews ----------------------------------------------------------------------------------------

private val previewInfo = Protocol.CameraInfo(
    quality = Protocol.QUALITY_1080P, fps = 30, zoomX100 = 100, zoomMinX100 = 60, zoomMaxX100 = 1000,
    ev = 1, evMin = -12, evMax = 12, evStepX100 = 33,
    flags = Protocol.CAM_TORCH_AVAILABLE or Protocol.CAM_HAS_AUTOFOCUS or Protocol.CAM_HAS_4K,
    width = 1920, height = 1080, actualFps = 30,
    fpsModes = listOf(Protocol.FPS_30 or Protocol.FPS_60 or Protocol.FPS_120, Protocol.FPS_30 or Protocol.FPS_60, Protocol.FPS_30),
)

@Preview(showBackground = true, widthDp = 360, heightDp = 780)
@Composable
private fun StreamingPreview() {
    MycamTheme(reducedMotion = true) {
        WebcamScreen(WebcamService.UiState(connected = true, streaming = true, resolution = "1920×1080", cameraInfo = previewInfo))
    }
}

@Preview(showBackground = true, widthDp = 360, heightDp = 780)
@Composable
private fun PausedPreview() {
    MycamTheme(reducedMotion = true) {
        WebcamScreen(WebcamService.UiState(connected = true, paused = true))
    }
}

@Preview(showBackground = true, widthDp = 360, heightDp = 780)
@Composable
private fun DisconnectedPreview() {
    MycamTheme(reducedMotion = true) { WebcamScreen(WebcamService.UiState()) }
}

@Preview(showBackground = true, widthDp = 360, heightDp = 1100)
@Composable
private fun SettingsPreview() {
    MycamTheme(reducedMotion = true) {
        WebcamScreen(
            WebcamService.UiState(
                connected = true, streaming = true, resolution = "1920×1080", cameraInfo = previewInfo,
                camera = io.github.whoissaaif.mycam.CameraStreamer.Settings(quality = Protocol.QUALITY_4K),
                wirelessOn = true, wirelessAddress = "192.168.1.23",
                pairedPcs = listOf(PairedPc("a1", "DESKTOP-ABC"), PairedPc("b2", "LAPTOP-XYZ")),
            ),
            initialTab = TAB_SETTINGS,
        )
    }
}

@Preview(showBackground = true, widthDp = 800, heightDp = 400)
@Composable
private fun LandscapePreview() {
    MycamTheme(reducedMotion = true) {
        WebcamScreen(WebcamService.UiState(connected = true, streaming = true, resolution = "1920×1080", cameraInfo = previewInfo))
    }
}

@Preview(showBackground = true, widthDp = 360, heightDp = 780)
@Composable
private fun PairingPreview() {
    MycamTheme(reducedMotion = true) {
        WebcamScreen(WebcamService.UiState(wirelessOn = true, pendingPc = "DESKTOP-ABC", pendingCode = "554294"))
    }
}
