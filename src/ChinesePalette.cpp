/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "ChinesePalette.h"

// Pale, low-chroma Song-inspired tints (flat row backgrounds). Names follow
// classical Chinese color vocabulary; values stay soft so rows stay readable.
static const ChineseColorSwatch gChinesePalette[] = {
    // whites / cool pale
    {"月白", 0xD6ECF0},   {"雪色", 0xEAF2F5},   {"霜色", 0xE8EEF0},   {"铅白", 0xF0EDE6},
    {"素", 0xF2F0EA},     {"荼白", 0xF3F0E8},   {"缟", 0xF0EBE0},
    // greens / cyan
    {"青白", 0xC0EBD7},   {"天水碧", 0xD5EBED}, {"艾绿", 0xD5EFDF},   {"竹青", 0xE2EAD9},
    {"水色", 0xDCE8E6},   {"湖绿", 0xC8E3D7},   {"葱青", 0xD4E8D0},   {"松花", 0xE5E8C9},
    {"豆青", 0xD5E4D0},   {"缥", 0xC9E0DD},     {"苍青", 0xD0DFE3},   {"碧色", 0xC9E6E0},
    {"翠涛", 0xC9E6D8},   {"青雀头黛", 0xD2DFD8},
    // blues
    {"天青", 0xD0E3EE},   {"霁青", 0xC5D8E8},   {"雨过天青", 0xC8D9E6}, {"东方既白", 0xD8E6EF},
    {"花青", 0xCDD9E8},   {"绀青", 0xD5DCE8},   {"沧浪", 0xD2E4EC},
    // pinks / purples
    {"藕荷", 0xF0E0E6},   {"藕色", 0xF5E6EA},   {"水红", 0xF2DDE3},   {"粉红", 0xF2E0E3},
    {"樱草", 0xEDE0EA},   {"丁香", 0xE8DEE8},   {"紫水晶", 0xE6DFEA}, {"紫苑", 0xE9E0EB},
    {"芙蓉", 0xF0DCE2},   {"十样锦", 0xF0E2E8},
    // yellows / warm pale
    {"缃色", 0xF4ECD4},   {"米色", 0xF3EBD9},   {"象牙白", 0xF2EBDD}, {"肉色", 0xF3E4D8},
    {"杏黄", 0xF2E6C9},   {"沙黄", 0xEFE6D0},   {"秋香", 0xE8E4C9},   {"姜黄", 0xF0E8C8},
    {"蜜合", 0xEFE6D4},   {"缟黄", 0xF0EAD8},
    // soft earth / gray-green
    {"牙色", 0xEEE6D8},   {"银鼠", 0xE4E4E0},   {"灰青", 0xDCE2E0},   {"黎", 0xE6E0D6},
};

int ChinesePaletteCount() {
    return dimof(gChinesePalette);
}

const ChineseColorSwatch& ChinesePaletteAt(int idx) {
    ReportIf(idx < 0 || idx >= ChinesePaletteCount());
    if (idx < 0 || idx >= ChinesePaletteCount()) {
        return gChinesePalette[0];
    }
    return gChinesePalette[idx];
}

// Pull a pastel away from white so Lucide strokes stay soft but visible.
static u8 InkChannel(u8 c) {
    constexpr float kFactor = 2.75f;
    int v = (int)(255.0f + (float)((int)c - 255) * kFactor);
    if (v < 48) {
        v = 48;
    }
    if (v > 196) {
        v = 196;
    }
    return (u8)v;
}

Color ChinesePaletteIconColorFromRgb(u32 rgb) {
    u8 r = InkChannel((u8)((rgb >> 16) & 0xff));
    u8 g = InkChannel((u8)((rgb >> 8) & 0xff));
    u8 b = InkChannel((u8)(rgb & 0xff));
    return MkRgb(r, g, b);
}

Color ChinesePaletteIconColor(int idx) {
    int n = ChinesePaletteCount();
    if (n <= 0) {
        return MkRgb(0x6B, 0x8E, 0x9B);
    }
    if (idx < 0) {
        idx = 0;
    }
    return ChinesePaletteIconColorFromRgb(ChinesePaletteAt(idx % n).rgb);
}
