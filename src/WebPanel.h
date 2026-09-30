/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;

void CreateWebPanel(MainWindow* win);
void DestroyWebPanel(MainWindow* win);
void RelayoutWebPanel(MainWindow* win);
void OnWebPanelToggle(MainWindow* win);
void CloseWebPanel(MainWindow* win);
void WebPanelOnDocumentChanged(MainWindow* win);
// Flush current book's AI open/closed (+ tab bindings) to pdf-map.json.
void WebPanelPersistActiveBookAiState(MainWindow* win);
// Restore last book's AI open/closed after CreateWebPanel / session tab load.
void DeferredApplyLastBookAi(MainWindow* win);
// Coalesced DeferredApplyLastBookAi: runs once input has been processed.
void ScheduleApplyBookAi(MainWindow* win);
void UpdateWebPanelDpi(MainWindow* win, int dpi);
void UpdateWebPanelTheme(MainWindow* win);
// Re-fetch header icons after the SVG icon cache was reset (theme / DPI).
void UpdateWebPanelIcons(MainWindow* win);
bool IsWebPanelVisible(MainWindow* win);

// Center Web browser surface (canvas XOR). Shares WebView2 env/CDP with AI panel.
void CreateWebBrowserPanel(MainWindow* win);
void DestroyWebBrowserPanel(MainWindow* win);
void RelayoutWebBrowserPanel(MainWindow* win);
bool IsWebBrowserPanelVisible(MainWindow* win);
// Instant canvas XOR WebView2 controllers (no deferred ghosting on fast switch).
void ApplyCenterContentSurface(MainWindow* win);
void OpenLibraryWebBook(MainWindow* win, i64 bookId);
// kind: LibraryBookKind as int (Pdf=0, Web=1)
void LibraryOnActiveBookChanged(MainWindow* win, i64 bookId, int kind);
// Always create a new browser tab for url (duplicates allowed).
void WebBrowserOpenUrlAsNewTab(MainWindow* win, Str url, Str title = {});
void WebBrowserShowPanel(MainWindow* win);
// Close the center-Web tab bound to this library web book (keeps the library row).
void WebBrowserCloseTabForLibraryBook(MainWindow* win, i64 bookId);

// Queue a NotebookLM add job and spawn the Node/Playwright helper (CDP 9224).
// pdfPath and/or sourceUrl (center Web URL); at least one required.
void WebPanelAddToNotebookLm(MainWindow* win, i64 bookId, Str pdfPath, Str sourceUrl, Str title);
void WebPanelAddPdfToNotebookLm(MainWindow* win, i64 bookId, Str pdfPath, Str title);
// Select only this PDF's source in NotebookLM (uncheck others) via Playwright.
void WebPanelSelectNotebookLmSource(MainWindow* win, i64 bookId, Str pdfPath, Str title);
// Clear per-PDF/Web AI panel tab id / url bindings.
void WebPanelClearPdfTabIds(i64 bookId);
void WebPanelClearPdfTabUrls(i64 bookId);
// Library context menu: inspect + clear companion AI tab bindings.
void WebPanelShowAiTabBindings(MainWindow* win, i64 bookId);
void WebPanelPollBridgeResults();
TempStr ScriptsWebviewDirTemp();
// Repo / SUMATRA_SCRIPTS checkout that contains package.json (may be empty).
TempStr FindRepoScriptsWebviewTemp();
void EnsureScriptsWebviewLayout();
void WebPanelSpawnScript(Str scriptName, Str extraArgs = {});
// -dbg-control TestWebPanel: show|hide|status|add|poll (AI/Cursor closed loop).
TempStr WebPanelDbgControlTemp(Str action, Str a, Str b, int n1, int n2, int* exitCodeOut);
