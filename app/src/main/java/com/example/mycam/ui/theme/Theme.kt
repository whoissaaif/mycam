package com.example.mycam.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable

// Windows 7 had no dark mode, so MyCam uses one light Aero scheme everywhere (no dynamic colour).
private val AeroColorScheme = lightColorScheme(
    primary = Aero.MainInstruction,
    onPrimary = Aero.Body,
    secondary = Aero.Link,
    background = Aero.Body,
    onBackground = Aero.Text,
    surface = Aero.Body,
    onSurface = Aero.Text,
    onSurfaceVariant = Aero.Subtle,
    outline = Aero.ButtonNormalBorder,
)

@Composable
fun MycamTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = AeroColorScheme, typography = Typography, content = content)
}
