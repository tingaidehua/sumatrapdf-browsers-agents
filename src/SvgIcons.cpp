/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Pixmap.h"

extern "C" {
#include <mupdf/fitz.h>
}

#include "ImageReader.h"
#include "Theme.h"
#include "SvgIcons.h"

// UI icons — Lucide (https://lucide.dev), ISC license.
// Prefer Lucide for any new gIcon*: copy the SVG from lucide.dev, keep
// viewBox 0 0 24 24, stroke currentColor, stroke-width 1.5.
// Filled (active) variants may set fill="currentColor" on the glyph paths.

// https://lucide.dev/icons/house
const char* gIconHome =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M15 21v-8a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v8" /> <path d="M3 10a2 2 0 0 1 .709-1.528l7-5.999a2 2 0 0 1 2.582 0l7 5.999A2 2 0 0 1 21 10v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" /> </svg>)";

// https://lucide.dev/icons/library
const char* gIconLibrary =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="m16 6 4 14" /> <path d="M12 6v14" /> <path d="M8 8v12" /> <path d="M4 4v16" /> </svg>)";

// https://lucide.dev/icons/bookmark
const char* gIconBookmarks =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="m19 21-7-4-7 4V5a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2v16z" /> </svg>)";

// https://lucide.dev/icons/plus
const char* gIconPlus =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M5 12h14" /> <path d="M12 5v14" /> </svg>)";

// https://lucide.dev/icons/message-circle
const char* gIconChat =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M7.9 20A9 9 0 1 0 4 16.1L2 22Z" /> </svg>)";

// https://lucide.dev/icons/crosshair
const char* gIconTargetFocus =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <circle cx="12" cy="12" r="10" /> <line x1="22" x2="18" y1="12" y2="12" /> <line x1="6" x2="2" y1="12" y2="12" /> <line x1="12" x2="12" y1="6" y2="2" /> <line x1="12" x2="12" y1="22" y2="18" /> </svg>)";

// https://lucide.dev/icons/notebook-pen
const char* gIconNotebookLm =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M13.4 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-7.4" /> <path d="M2 6h4" /> <path d="M2 10h4" /> <path d="M2 14h4" /> <path d="M2 18h4" /> <path d="M21.378 5.626a1 1 0 1 0-3.004-3.004l-5.01 5.012a2 2 0 0 0-.506.854l-.837 2.87a.5.5 0 0 0 .62.62l2.87-.837a2 2 0 0 0 .854-.506z" /> </svg>)";

// https://lucide.dev/icons/app-window
const char* gIconTabs =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <rect x="2" y="4" width="20" height="16" rx="2" /> <path d="M10 4v4" /> <path d="M2 8h20" /> <path d="M6 4v4" /> </svg>)";

// https://lucide.dev/icons/history
const char* gIconHistory =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8" /> <path d="M3 3v5h5" /> <path d="M12 7v5l4 2" /> </svg>)";

// https://lucide.dev/icons/puzzle
const char* gIconExtensions =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M15.39 4.39a1 1 0 0 0 1.68-.474 2.5 2.5 0 1 1 3.014 3.015 1 1 0 0 0-.474 1.68l1.683 1.682a2.414 2.414 0 0 1 0 3.414L19.61 15.39a1 1 0 0 1-1.68-.474 2.5 2.5 0 1 0-3.014 3.015 1 1 0 0 1 .474 1.68l-1.683 1.682a2.414 2.414 0 0 1-3.414 0L8.61 19.61a1 1 0 0 0-1.68.474 2.5 2.5 0 1 1-3.014-3.015 1 1 0 0 0 .474-1.68l-1.683-1.682a2.414 2.414 0 0 1 0-3.414L4.39 8.61a1 1 0 0 1 1.68.474 2.5 2.5 0 1 0 3.014-3.015 1 1 0 0 1-.474-1.68l1.683-1.682a2.414 2.414 0 0 1 3.414 0z" /> </svg>)";

// https://lucide.dev/icons/refresh-cw
const char* gIconRefresh =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8" /> <path d="M21 3v5h-5" /> <path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16" /> <path d="M8 16H3v5" /> </svg>)";

// https://lucide.dev/icons/info
const char* gIconInfo =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <circle cx="12" cy="12" r="10" /> <path d="M12 16v-4" /> <path d="M12 8h.01" /> </svg>)";

// https://lucide.dev/icons/folder-open
const char* gIconFileOpen =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="m6 14 1.5-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.54 6a2 2 0 0 1-1.95 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2" /> </svg>)";

// https://lucide.dev/icons/printer
const char* gIconPrint =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M6 18H4a2 2 0 0 1-2-2v-5a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v5a2 2 0 0 1-2 2h-2" /> <path d="M6 9V3a1 1 0 0 1 1-1h10a1 1 0 0 1 1 1v6" /> <rect x="6" y="14" width="12" height="8" rx="1" /> </svg>)";

// https://lucide.dev/icons/arrow-left
const char* gIconPagePrev =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="m12 19-7-7 7-7" /> <path d="M19 12H5" /> </svg>)";

// https://lucide.dev/icons/arrow-right
const char* gIconPageNext =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M5 12h14" /> <path d="m12 5 7 7-7 7" /> </svg>)";

// https://lucide.dev/icons/rows-2
const char* gIconLayoutContinuous =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <rect width="18" height="18" x="3" y="3" rx="2" /> <path d="M3 12h18" /> </svg>)";

// https://lucide.dev/icons/square
const char* gIconLayoutSinglePage =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <rect width="18" height="18" x="3" y="3" rx="2" /> </svg>)";

// https://lucide.dev/icons/chevron-left
const char* gIconSearchPrev =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="m15 18-6-6 6-6" /> </svg>)";

// https://lucide.dev/icons/chevron-right
const char* gIconSearchNext =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="m9 18 6-6-6-6" /> </svg>)";

// https://lucide.dev/icons/case-sensitive
const char* gIconMatchCase =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="m3 15 4-8 4 8" /> <path d="M4 13h6" /> <circle cx="18" cy="12" r="3" /> <path d="M21 9v6" /> </svg>)";

// https://lucide.dev/icons/whole-word
const char* gIconMatchWholeWord =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <circle cx="7" cy="12" r="3" /> <path d="M10 9v6" /> <circle cx="17" cy="12" r="3" /> <path d="M14 7v8" /> <path d="M22 17v1c0 .5-.5 1-1 1H3c-.5 0-1-.5-1-1v-1" /> </svg>)";

// https://lucide.dev/icons/zoom-in
const char* gIconZoomIn =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <circle cx="11" cy="11" r="8" /> <line x1="21" x2="16.65" y1="21" y2="16.65" /> <line x1="11" x2="11" y1="8" y2="14" /> <line x1="8" x2="14" y1="11" y2="11" /> </svg>)";

// https://lucide.dev/icons/zoom-out
const char* gIconZoomOut =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <circle cx="11" cy="11" r="8" /> <line x1="21" x2="16.65" y1="21" y2="16.65" /> <line x1="8" x2="14" y1="11" y2="11" /> </svg>)";

// https://lucide.dev/icons/save
const char* gIconSave =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z" /> <path d="M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7" /> <path d="M7 3v4a1 1 0 0 0 1 1h7" /> </svg>)";

// https://lucide.dev/icons/rotate-ccw
const char* gIconRotateLeft =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8" /> <path d="M3 3v5h5" /> </svg>)";

// https://lucide.dev/icons/rotate-cw
const char* gIconRotateRight =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8" /> <path d="M21 3v5h-5" /> </svg>)";

// https://lucide.dev/icons/volume-2
const char* gIconSpeak =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z" /> <path d="M16 9a5 5 0 0 1 0 6" /> <path d="M19.364 18.364a9 9 0 0 0 0-12.728" /> </svg>)";

// https://lucide.dev/icons/pause
const char* gIconPauseSpeaking =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <rect x="14" y="4" width="4" height="16" rx="1" /> <rect x="6" y="4" width="4" height="16" rx="1" /> </svg>)";

// https://lucide.dev/icons/undo-2
const char* gIconNavigateBack =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M9 14 4 9l5-5" /> <path d="M4 9h10.5a5.5 5.5 0 0 1 5.5 5.5a5.5 5.5 0 0 1-5.5 5.5H11" /> </svg>)";

// https://lucide.dev/icons/redo-2
const char* gIconNavigateForward =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="m15 14 5-5-5-5" /> <path d="M20 9H9.5A5.5 5.5 0 0 0 4 14.5A5.5 5.5 0 0 0 9.5 20H13" /> </svg>)";

// https://lucide.dev/icons/search
const char* gIconSearch =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <circle cx="11" cy="11" r="8" /> <path d="m21 21-4.3-4.3" /> </svg>)";

// https://lucide.dev/icons/chevron-up
const char* gIconChevronUp =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="m18 15-6-6-6 6" /> </svg>)";

// https://lucide.dev/icons/chevron-down
const char* gIconChevronDown =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="m6 9 6 6 6-6" /> </svg>)";

// https://lucide.dev/icons/x
const char* gIconClose =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M18 6 6 18" /> <path d="m6 6 12 12" /> </svg>)";

// https://lucide.dev/icons/maximize-2
const char* gIconArrowsDiagonal =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <polyline points="15 3 21 3 21 9" /> <polyline points="9 21 3 21 3 15" /> <line x1="21" x2="14" y1="3" y2="10" /> <line x1="3" x2="10" y1="21" y2="14" /> </svg>)";

// https://lucide.dev/icons/minimize-2
const char* gIconArrowsDiagonalMinimize =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <polyline points="4 14 10 14 10 20" /> <polyline points="20 10 14 10 14 4" /> <line x1="14" x2="21" y1="10" y2="3" /> <line x1="3" x2="10" y1="21" y2="14" /> </svg>)";

// https://lucide.dev/icons/list
const char* gIconHomeList =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M3 12h.01" /> <path d="M3 18h.01" /> <path d="M3 6h.01" /> <path d="M8 12h13" /> <path d="M8 18h13" /> <path d="M8 6h13" /> </svg>)";

// https://lucide.dev/icons/layout-grid
const char* gIconHomeThumbnails =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <rect width="7" height="7" x="3" y="3" rx="1" /> <rect width="7" height="7" x="14" y="3" rx="1" /> <rect width="7" height="7" x="14" y="14" rx="1" /> <rect width="7" height="7" x="3" y="14" rx="1" /> </svg>)";

// https://lucide.dev/icons/pin
const char* gIconPin =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M12 17v5" /> <path d="M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z" /> </svg>)";

// https://lucide.dev/icons/pin (filled / active)
const char* gIconPinFilled =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="currentColor" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" > <path d="M12 17v5" /> <path d="M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z" /> </svg>)";

// A custom ToolbarSvgIcon comes from the settings file, so it can be malformed:
// a typo, or the file caught half-written by the settings watcher while the user
// is editing it. mupdf signals that by throwing, and an uncaught mupdf exception
// aborts the whole process, so everything here has to be inside fz_try.
static fz_pixmap* RenderSvgToFzPixmap(fz_context* ctx, Str svgData, int dx, int dy, Color fgCol, Color bgCol) {
    TempStr strokeCol = SerializeColorTemp(fgCol);
    TempStr fillCol = SerializeColorTemp(bgCol);
    TempStr fillColRepl = str::JoinTemp(StrL("fill=\""), fillCol, StrL("\""));
    TempStr svg = str::ReplaceTemp(svgData, StrL("currentColor"), strokeCol);
    svg = str::ReplaceTemp(svg, StrL(R"(fill="none")"), fillColRepl);

    fz_buffer* buf = nullptr;
    fz_image* image = nullptr;
    fz_pixmap* pixmap = nullptr;
    fz_var(buf);
    fz_var(image);
    fz_var(pixmap);
    fz_try(ctx) {
        buf = fz_new_buffer_from_copied_data(ctx, (u8*)svg.s, svg.len);
        image = fz_new_image_from_svg(ctx, buf, nullptr, nullptr);
        image->w = dx;
        image->h = dy;
        pixmap = fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
    }
    fz_always(ctx) {
        fz_drop_image(ctx, image);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logf("GetCachedPixmapForSvg: rendering svg icon failed with: '%s'\n", Str(fz_caught_message(ctx)));
        return nullptr;
    }
    return pixmap;
}

static void BlitFzPixmapBgra(u8* dstSamples, ptrdiff_t dstStride, fz_pixmap* src, Color bgCol) {
    int dx = src->w;
    int dy = src->h;
    int srcN = src->n;
    auto srcStride = src->stride;
    u8 r, g, b;
    UnpackColor(bgCol, r, g, b);
    for (size_t y = 0; y < (size_t)dy; y++) {
        u8* s = src->samples + (srcStride * y);
        u8* d = dstSamples + (dstStride * y);
        for (int x = 0; x < dx; x++) {
            bool isTransparent = (s[0] == r) && (s[1] == g) && (s[2] == b);
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = isTransparent ? 0 : 0xff;
            d += 4;
            s += srcN;
        }
    }
}

// BGRA DIB, alpha-premultiplied, transparent where the SVG left the background.
static Pixmap* RenderSvgToPixmap(Str svgData, int dx, int dy, Color fgCol, Color bgCol) {
    Pixmap* px = AllocPixmapDIB(dx, dy);
    if (!px) {
        return nullptr;
    }
    memset(px->data, 0, (size_t)px->stride * (size_t)dy);
    px->premultiplied = true;

    fz_context* ctx = fz_new_context_windows();
    fz_pixmap* pixmap = RenderSvgToFzPixmap(ctx, svgData, dx, dy, fgCol, bgCol);
    if (pixmap) {
        BlitFzPixmapBgra(px->data, px->stride, pixmap, bgCol);
        u8* row = px->data;
        for (int y = 0; y < dy; y++) {
            u8* d = row;
            for (int x = 0; x < dx; x++) {
                if (d[3] == 0) {
                    d[0] = d[1] = d[2] = 0;
                }
                d += 4;
            }
            row += px->stride;
        }
        fz_drop_pixmap(ctx, pixmap);
    }
    fz_drop_context_windows(ctx);
    return px;
}

// Super-set of the old GetPixmapForIcon / SelToolbarIcon caches: keyed by
// SVG bytes (built-in gIcon* or a user-provided string), size, and colors.
struct SvgPixmapCacheEntry {
    SvgPixmapCacheEntry* next = nullptr;
    Str svg; // owned
    int dx = 0;
    int dy = 0;
    Color fg = 0;
    Color bg = 0;
    Pixmap* pixmap = nullptr; // owned

    ~SvgPixmapCacheEntry() {
        str::Free(svg);
        FreePixmap(pixmap);
    }
};

static SvgPixmapCacheEntry* gSvgPixmapCache = nullptr;

// Render `svg` at dx脳dy in fg/bg (theme text/control colors if unset).
// The Pixmap belongs to the cache until DestroySvgPixmapIconsCache().
Pixmap* GetCachedPixmapForSvg(Str svg, int dx, int dy, Color fg, Color bg) {
    if (str::IsEmptyOrWhiteSpace(svg) || dx <= 0 || dy <= 0) {
        return nullptr;
    }
    if (fg == kColorUnset) {
        fg = ThemeWindowTextColor();
    }
    if (bg == kColorUnset) {
        bg = ThemeControlBackgroundColor();
    }
    for (SvgPixmapCacheEntry* e = gSvgPixmapCache; e; e = e->next) {
        if (e->dx == dx && e->dy == dy && e->fg == fg && e->bg == bg && str::Eq(e->svg, svg)) {
            return e->pixmap;
        }
    }
    Pixmap* px = RenderSvgToPixmap(svg, dx, dy, fg, bg);
    if (!px) {
        return nullptr;
    }
    auto* e = new SvgPixmapCacheEntry();
    e->svg = str::Dup(svg);
    e->dx = dx;
    e->dy = dy;
    e->fg = fg;
    e->bg = bg;
    e->pixmap = px;
    ListInsertFront(&gSvgPixmapCache, e);
    return px;
}

// Pixmaps from earlier theme/DPI generations. Controls hold raw Pixmap* from
// GetCachedPixmapForSvg() and not all of them are refreshed on theme/DPI change,
// so freeing immediately left dangling pointers (AV in PixmapAsPremulBgra while
// painting a header button). Icons are tiny; keep them until shutdown.
static SvgPixmapCacheEntry* gRetiredSvgPixmaps = nullptr;

static void RetireSvgPixmapCache() {
    SvgPixmapCacheEntry* e = gSvgPixmapCache;
    while (e) {
        SvgPixmapCacheEntry* next = e->next;
        e->next = gRetiredSvgPixmaps;
        gRetiredSvgPixmaps = e;
        e = next;
    }
    gSvgPixmapCache = nullptr;
}

// Theme and DPI: subsequent lookups render in the current colors/size.
void DestroySvgPixmapIconsCache() {
    RetireSvgPixmapCache();
}

void FreeSvgPixmapIconsCacheAtShutdown() {
    ListDelete(gSvgPixmapCache);
    gSvgPixmapCache = nullptr;
    ListDelete(gRetiredSvgPixmaps);
    gRetiredSvgPixmaps = nullptr;
}
