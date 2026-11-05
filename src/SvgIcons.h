/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Built-in toolbar / sidebar icons are Lucide SVGs (https://lucide.dev).
// When adding a new gIcon*, use Lucide: 24x24, stroke currentColor, width 1.5.

extern const char* gIconFileOpen;
extern const char* gIconPrint;
extern const char* gIconPagePrev;
extern const char* gIconPageNext;
extern const char* gIconLayoutContinuous;
extern const char* gIconLayoutSinglePage;
extern const char* gIconZoomOut;
extern const char* gIconZoomIn;
extern const char* gIconSearchPrev;
extern const char* gIconSearchNext;
extern const char* gIconMatchCase;
extern const char* gIconSave;
extern const char* gIconRotateLeft;
extern const char* gIconRotateRight;
extern const char* gIconSpeak;
extern const char* gIconPauseSpeaking;
extern const char* gIconNavigateBack;
extern const char* gIconNavigateForward;
extern const char* gIconSearch;
extern const char* gIconChevronUp;
extern const char* gIconChevronDown;
extern const char* gIconClose;
extern const char* gIconArrowsDiagonal;
extern const char* gIconArrowsDiagonalMinimize;
extern const char* gIconMatchWholeWord;
extern const char* gIconHomeList;
extern const char* gIconHomeThumbnails;
extern const char* gIconPin;
extern const char* gIconPinFilled;
extern const char* gIconHome;
extern const char* gIconLibrary;
extern const char* gIconBookmarks;
extern const char* gIconPlus;
extern const char* gIconChat;
// Focus NotebookLM chat on the current PDF only (lucide: crosshair).
extern const char* gIconTargetFocus;
// NotebookLM tab for the current PDF (lucide: notebook-pen).
extern const char* gIconNotebookLm;
// Browser-style tabs list (lucide: app-window).
extern const char* gIconTabs;
// Browsing history (lucide: history).
extern const char* gIconHistory;
// Extensions menu (lucide: puzzle).
extern const char* gIconExtensions;
// Reload / refresh (lucide: refresh-cw).
extern const char* gIconRefresh;
// Developer / CDP info (lucide: info).
extern const char* gIconInfo;

struct Pixmap;

Pixmap* GetCachedPixmapForSvg(Str svg, int dx, int dy, Color fg = kColorUnset, Color bg = kColorUnset);
void DestroySvgPixmapIconsCache();
void FreeSvgPixmapIconsCacheAtShutdown();
