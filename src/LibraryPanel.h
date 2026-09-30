/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;

// Left inset of the library pane so the frame's HTLEFT hit-test is reachable.
constexpr int kLibraryLeftGutterDip = 3;

void CreateLibraryPanel(MainWindow* win);
void LayoutLibraryPanel(MainWindow* win);
void UpdateLibraryPanelText(MainWindow* win);
// Re-fetch header icons after the SVG icon cache was reset (theme / DPI).
void UpdateLibraryPanelIcons(MainWindow* win);
void RefreshLibraryPanel(MainWindow* win);
void RefreshLibraryPanels();
void SyncLibrarySelection(MainWindow* win);
// Persist expand/collapse + last book id (call after selection changes).
void LibrarySaveUiState(MainWindow* win);
// Update library row for a web book to match the browser tab title (in-place).
void LibraryUpdateWebBookTabTitle(i64 bookId, Str title);
// target=_blank from center Web → new library web book + center browser tab (never AI).
void LibraryOpenWebUrlInBrowser(MainWindow* win, Str url);
void SetLibraryPanelVisible(MainWindow* win, bool visible);
TempStr LibraryDbgControlTemp(Str action, Str a, Str b, int n1, int n2, int* exitCodeOut);
