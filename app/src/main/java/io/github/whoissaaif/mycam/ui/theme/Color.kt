package io.github.whoissaaif.mycam.ui.theme

import androidx.compose.ui.graphics.Color

// Windows 7 "Aero" design tokens (IMPROVEMENTS.md section 7.2). Shared with the PC settings window
// (pc/companion/settings_window.cpp), so both apps look like one product.
object Aero {
    // Glass frame / header.
    val GlassTop = Color(0xFFC9DDF3)
    val GlassMid = Color(0xFFA9C6EA)
    val GlassBottom = Color(0xFF8FB2DD)
    val GlassEdge = Color(0xFF3E5F8A)

    // Content.
    val Body = Color(0xFFFFFFFF)
    val CommandArea = Color(0xFFF0F0F0)
    val CommandLine = Color(0xFFDFDFDF)
    val Rule = Color(0xFFE2E2E2)
    val MainInstruction = Color(0xFF003399)
    val Heading = Color(0xFF1E3287)
    val Text = Color(0xFF000000)
    val Subtle = Color(0xFF5A5A5A)
    val Link = Color(0xFF0066CC)

    // Buttons: two-tone gloss (top half light, bottom half darker), per state.
    val ButtonNormal = listOf(Color(0xFFF2F2F2), Color(0xFFEBEBEB), Color(0xFFDDDDDD), Color(0xFFCFCFCF))
    val ButtonNormalBorder = Color(0xFF707070)
    val ButtonPressed = listOf(Color(0xFFE5F4FC), Color(0xFFC4E5F6), Color(0xFF98D1EF), Color(0xFF68B3DB))
    val ButtonPressedBorder = Color(0xFF2C628B)
    val ButtonDisabled = Color(0xFFF4F4F4)
    val ButtonDisabledBorder = Color(0xFFADB2B5)
    val ButtonDisabledText = Color(0xFF838383)

    // Command link hover/press box (Win7 command links).
    val CommandLinkFill = Color(0xFFF2F8FD)
    val CommandLinkBorder = Color(0xFFC6DCF0)

    // "Live" green (Win7 progress bar) and status badges.
    val LiveTop = Color(0xFF8BE07A)
    val LiveMid = Color(0xFF37C12B)
    val LiveLow = Color(0xFF06B025)
    val LiveBottom = Color(0xFF3CCB47)
    val LiveBorder = Color(0xFF0A7A1A)
    val PausedLight = Color(0xFFFFE482)
    val PausedDark = Color(0xFFD68000)
    val ResumeLight = Color(0xFF96F078)
    val ResumeDark = Color(0xFF108C1E)
}
