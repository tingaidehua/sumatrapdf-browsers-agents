/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Shared Song-inspired Chinese pale palette: library row backgrounds,
// toolbar / AI header Lucide icon strokes (via ChinesePaletteIconColor).

#pragma once

struct ChineseColorSwatch {
    const char* name; // classical Chinese color name
    u32 rgb;          // 0x00RRGGBB, pale tint suitable as a row background
};

int ChinesePaletteCount();
const ChineseColorSwatch& ChinesePaletteAt(int idx);

// Soft ink stroke from a pastel swatch (same hue, readable on white UI).
Color ChinesePaletteIconColor(int idx);
Color ChinesePaletteIconColorFromRgb(u32 rgb);
