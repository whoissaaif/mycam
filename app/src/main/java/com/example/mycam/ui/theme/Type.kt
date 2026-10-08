package com.example.mycam.ui.theme

import androidx.compose.material3.Typography
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp
import com.example.mycam.R

// Selawik: Microsoft's open-source (OFL) Segoe UI substitute, for the Windows 7 look.
// License: assets/licenses/Selawik-OFL.txt
val Selawik = FontFamily(
    Font(R.font.selawik_light, FontWeight.Light),
    Font(R.font.selawik_regular, FontWeight.Normal),
    Font(R.font.selawik_semibold, FontWeight.SemiBold),
    Font(R.font.selawik_bold, FontWeight.Bold),
)

// Win7 type ramp, scaled for a phone: title, main instruction, body, caption.
val Typography = Typography(
    titleLarge = TextStyle(fontFamily = Selawik, fontWeight = FontWeight.Normal, fontSize = 20.sp, lineHeight = 26.sp),
    headlineSmall = TextStyle(fontFamily = Selawik, fontWeight = FontWeight.Normal, fontSize = 24.sp, lineHeight = 30.sp),
    titleMedium = TextStyle(fontFamily = Selawik, fontWeight = FontWeight.SemiBold, fontSize = 16.sp, lineHeight = 22.sp),
    bodyLarge = TextStyle(fontFamily = Selawik, fontWeight = FontWeight.Normal, fontSize = 16.sp, lineHeight = 22.sp),
    bodyMedium = TextStyle(fontFamily = Selawik, fontWeight = FontWeight.Normal, fontSize = 14.sp, lineHeight = 20.sp),
    labelLarge = TextStyle(fontFamily = Selawik, fontWeight = FontWeight.Normal, fontSize = 15.sp, lineHeight = 20.sp),
    labelSmall = TextStyle(fontFamily = Selawik, fontWeight = FontWeight.Bold, fontSize = 11.sp, lineHeight = 14.sp),
)
