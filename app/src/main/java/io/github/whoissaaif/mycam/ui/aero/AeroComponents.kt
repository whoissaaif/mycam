package io.github.whoissaaif.mycam.ui.aero

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.Shadow
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import io.github.whoissaaif.mycam.ui.theme.Aero

// Windows 7 "Aero" building blocks for Compose (IMPROVEMENTS.md section 7). The PC settings window
// draws the same parts with Direct2D (pc/companion/settings_window.cpp).

/** Two-tone gloss: lighter top half, sharp split, darker bottom half. */
private fun gloss(c: List<Color>) = Brush.verticalGradient(
    0f to c[0], 0.5f to c[1], 0.5f to c[2], 1f to c[3],
)

/** Glass title area with a soft aurora sheen, drawn behind the status bar. */
@Composable
fun GlassHeader(title: String, iconRes: Int, modifier: Modifier = Modifier) {
    Box(
        modifier
            .fillMaxWidth()
            .drawBehind {
                drawRect(Brush.verticalGradient(0f to Aero.GlassTop, 0.35f to Aero.GlassMid, 1f to Aero.GlassBottom))
                // Aurora sheens.
                drawCircle(
                    Brush.radialGradient(listOf(Color.White.copy(alpha = 0.55f), Color.Transparent),
                        center = Offset(size.width * 0.28f, size.height * 0.2f), radius = size.width * 0.45f),
                    radius = size.width * 0.45f, center = Offset(size.width * 0.28f, size.height * 0.2f),
                )
                drawCircle(
                    Brush.radialGradient(listOf(Color.White.copy(alpha = 0.35f), Color.Transparent),
                        center = Offset(size.width * 0.85f, size.height * 0.7f), radius = size.width * 0.35f),
                    radius = size.width * 0.35f, center = Offset(size.width * 0.85f, size.height * 0.7f),
                )
                drawLine(Color.White.copy(alpha = 0.8f), Offset(0f, 1f), Offset(size.width, 1f))
                drawLine(Aero.GlassEdge, Offset(0f, size.height - 1f), Offset(size.width, size.height - 1f), strokeWidth = 2f)
            }
            .windowInsetsPadding(WindowInsets.statusBars)
            .padding(horizontal = 16.dp, vertical = 12.dp),
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Image(painterResource(iconRes), contentDescription = null, modifier = Modifier.size(32.dp))
            Spacer(Modifier.width(10.dp))
            // Win7 caption text: black with a soft white glow.
            Text(
                title,
                style = MaterialTheme.typography.titleLarge.copy(
                    shadow = Shadow(Color.White, Offset.Zero, blurRadius = 14f),
                ),
                color = Aero.Text,
            )
        }
    }
}

/** Glossy Aero push button. [selected] shows the pressed (blue) look, for toggle/segment buttons. */
@Composable
fun AeroButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    selected: Boolean = false,
    enabled: Boolean = true,
    shape: RoundedCornerShape = RoundedCornerShape(3.dp),
) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val lit = selected || pressed
    Box(
        modifier
            .heightIn(min = 44.dp)
            .clip(shape)
            .background(
                if (!enabled) Brush.verticalGradient(listOf(Aero.ButtonDisabled, Aero.ButtonDisabled))
                else gloss(if (lit) Aero.ButtonPressed else Aero.ButtonNormal),
            )
            .border(1.dp, Color.White.copy(alpha = if (lit || !enabled) 0.3f else 0.8f), shape)
            .border(
                1.dp,
                when {
                    !enabled -> Aero.ButtonDisabledBorder
                    lit -> Aero.ButtonPressedBorder
                    else -> Aero.ButtonNormalBorder
                },
                shape,
            )
            .clickable(interaction, indication = null, enabled = enabled, role = Role.Button, onClick = onClick)
            .padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text,
            style = MaterialTheme.typography.labelLarge,
            color = if (enabled) Aero.Text else Aero.ButtonDisabledText,
            textAlign = TextAlign.Center,
        )
    }
}

/** Two Aero buttons joined into a segmented control; [selectedIndex] looks pressed. */
@Composable
fun AeroSegmented(
    options: List<String>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    enabled: (Int) -> Boolean = { true },
) {
    Row(modifier) {
        options.forEachIndexed { i, label ->
            val shape = when (i) {
                0 -> RoundedCornerShape(topStart = 3.dp, bottomStart = 3.dp)
                options.lastIndex -> RoundedCornerShape(topEnd = 3.dp, bottomEnd = 3.dp)
                else -> RoundedCornerShape(0.dp)
            }
            AeroButton(
                label, onClick = { onSelect(i) }, selected = i == selectedIndex, shape = shape, enabled = enabled(i),
                modifier = Modifier.weight(1f),
            )
        }
    }
}

enum class BadgeKind { Pause, Resume }

/** Glossy round badge (like the tray icon badges): amber with pause bars, or green with a play arrow. */
@Composable
fun GlossyBadge(kind: BadgeKind, size: Dp, modifier: Modifier = Modifier) {
    Canvas(modifier.size(size)) {
        val r = this.size.minDimension / 2
        val c = center
        val (light, dark) = if (kind == BadgeKind.Pause) Aero.PausedLight to Aero.PausedDark
        else Aero.ResumeLight to Aero.ResumeDark
        drawCircle(Color.White, r, c)
        val inner = r * 0.9f
        drawCircle(
            Brush.radialGradient(listOf(light, dark), center = Offset(c.x - inner * 0.3f, c.y - inner * 0.45f), radius = inner * 1.6f),
            inner, c,
        )
        if (kind == BadgeKind.Pause) {
            val w = inner * 0.24f
            val h = inner * 0.95f
            drawRect(Color.White, Offset(c.x - inner * 0.42f, c.y - h / 2), Size(w, h))
            drawRect(Color.White, Offset(c.x + inner * 0.18f, c.y - h / 2), Size(w, h))
        } else {
            val p = Path().apply {
                moveTo(c.x - inner * 0.28f, c.y - inner * 0.48f)
                lineTo(c.x + inner * 0.52f, c.y)
                lineTo(c.x - inner * 0.28f, c.y + inner * 0.48f)
                close()
            }
            drawPath(p, Color.White)
        }
        glossOver(c, inner)
    }
}

private fun DrawScope.glossOver(c: Offset, r: Float) {
    drawOval(
        Brush.verticalGradient(listOf(Color.White.copy(alpha = 0.55f), Color.Transparent), startY = c.y - r * 0.92f, endY = c.y + r * 0.08f),
        topLeft = Offset(c.x - r * 0.78f, c.y - r * 0.92f), size = Size(r * 1.56f, r * 1.0f),
    )
}

/**
 * Win7 command link: a large tappable row with an icon, a title and a description (the style Windows 7
 * used for the main choices in its dialogs).
 */
@Composable
fun CommandLink(
    title: String,
    description: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    icon: @Composable () -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val shape = RoundedCornerShape(3.dp)
    Row(
        modifier
            .fillMaxWidth()
            .clip(shape)
            .background(if (pressed) Aero.ButtonPressed[0] else Aero.CommandLinkFill)
            .border(1.dp, if (pressed) Aero.ButtonPressedBorder else Aero.CommandLinkBorder, shape)
            .clickable(interaction, indication = null, enabled = enabled, role = Role.Button, onClick = onClick)
            .padding(horizontal = 14.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        icon()
        Spacer(Modifier.width(14.dp))
        Column {
            Text(title, style = MaterialTheme.typography.titleMedium.copy(fontWeight = FontWeight.Normal, fontSize = MaterialTheme.typography.titleLarge.fontSize),
                color = if (enabled) Color(0xFF151C55) else Aero.ButtonDisabledText)
            Text(description, style = MaterialTheme.typography.bodyMedium, color = Aero.Subtle)
        }
    }
}

/** "LIVE" pill in the Win7 progress-bar green. */
@Composable
fun LivePill(modifier: Modifier = Modifier) {
    val shape = RoundedCornerShape(50)
    Box(
        modifier
            .clip(shape)
            .background(gloss(listOf(Aero.LiveTop, Aero.LiveMid, Aero.LiveLow, Aero.LiveBottom)))
            .border(1.dp, Aero.LiveBorder, shape)
            .padding(horizontal = 9.dp, vertical = 2.dp),
    ) {
        Text("LIVE", style = MaterialTheme.typography.labelSmall, color = Color.White)
    }
}

/** Section heading with a light rule to the right, like Win7 Control Panel groups. */
@Composable
fun SectionHeading(text: String, modifier: Modifier = Modifier) {
    Row(modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(text, style = MaterialTheme.typography.titleMedium, color = Aero.Heading)
        Spacer(Modifier.width(10.dp))
        Box(Modifier.weight(1f).height(1.dp).background(Aero.Rule))
    }
}

/** Gray strip at the bottom of a Win7 dialog. */
@Composable
fun CommandArea(modifier: Modifier = Modifier, content: @Composable RowScope.() -> Unit) {
    Column(modifier.fillMaxWidth().background(Aero.CommandArea)) {
        Box(Modifier.fillMaxWidth().height(1.dp).background(Aero.CommandLine))
        Row(
            Modifier
                .fillMaxWidth()
                .windowInsetsPadding(WindowInsets.navigationBars)
                .padding(horizontal = 20.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            content = content,
        )
    }
}

/** Win7 checkbox: 18 dp box with a soft inner gradient and a dark-blue check; the whole row is tappable. */
@Composable
fun AeroCheckbox(
    text: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    Row(
        modifier
            .heightIn(min = 44.dp)
            .clickable(enabled = enabled, role = Role.Checkbox) { onCheckedChange(!checked) },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Canvas(Modifier.size(18.dp)) {
            val s = size.minDimension
            drawRect(Color.White)
            drawRect(
                if (!enabled) Brush.verticalGradient(listOf(Aero.ButtonDisabled, Aero.ButtonDisabled))
                else Brush.verticalGradient(listOf(Color(0xFFCBCFD5), Color(0xFFF6F6F6))),
                topLeft = Offset(s * 0.15f, s * 0.15f), size = Size(s * 0.7f, s * 0.7f),
            )
            drawRect(
                if (enabled) Aero.ButtonNormalBorder else Aero.ButtonDisabledBorder,
                style = androidx.compose.ui.graphics.drawscope.Stroke(width = 1.dp.toPx()),
            )
            if (checked) {
                val p = Path().apply {
                    moveTo(s * 0.22f, s * 0.5f)
                    lineTo(s * 0.42f, s * 0.72f)
                    lineTo(s * 0.8f, s * 0.24f)
                }
                drawPath(
                    p, if (enabled) Color(0xFF1B3D82) else Aero.ButtonDisabledText,
                    style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.4.dp.toPx()),
                )
            }
        }
        Spacer(Modifier.width(10.dp))
        Text(text, style = MaterialTheme.typography.bodyLarge, color = if (enabled) Aero.Text else Aero.ButtonDisabledText)
    }
}
