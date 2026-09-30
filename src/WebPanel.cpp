/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/Win.h"
#include "base/Http.h"
#include "base/Pixmap.h"
#include "base/UITask.h"
#include "base/DirScan.h"

#include "gui/Dpi.h"
#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/VirtCtrl.h"
#include "gui/GuiColors.h"
#include "gui/win/WebView.h"

#include "Settings.h"
#include "AppSettings.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "Theme.h"
#include "DarkMode_win.h"
#include "SvgIcons.h"
#include "Translations.h"
#include "ImageReader.h"
#include "AIChatCommon.h"
#include "AIChatPanel.h"
#include "WebPanel.h"
#include "AppTools.h"
#include "Library.h"
#include "LibraryStore.h"
#include "LibraryPanel.h"
#include "base/JsonParser.h"

#include <psapi.h>
#include <tlhelp32.h>

namespace {

constexpr int kDefaultCdpPort = 9224;      // Browser-AIChat (NotebookLM / Playwright)
constexpr int kLibraryCdpPort = 9225;      // Browser-Library (center Web)
// Match anything-copilot: iPhone Safari UA + narrow viewport (no hybrid desktop)
static const char* kMobileUserAgent =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 16_6 like Mac OS X) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/16.6 Mobile/15E148 Safari/604.1";
// Center browser: Chrome desktop UA (no "Edg/") — Edge UA makes Gemini / AI Studio
// pick a broken font path; Chrome spoof fixes garbled / fuzzy CJK text.
static const char* kChromeDesktopUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/147.0.0.0 Safari/537.36";

static const char* kMobileViewportScript =
    R"JS((function(){
  try {
    var m = document.querySelector('meta[name="viewport"]');
    if (!m) {
      m = document.createElement('meta');
      m.setAttribute('name', 'viewport');
      (document.head || document.documentElement).appendChild(m);
    }
    m.setAttribute('content', 'width=device-width, initial-scale=1, maximum-scale=1, viewport-fit=cover');
  } catch (e) {}
})())JS";

struct DefaultBookmark {
    const char* title;
    const char* url;
};

// Featured sites (ZIZIYI / anything-copilot style) + NotebookLM
static const DefaultBookmark kDefaultBookmarks[] = {
    {"Gemini Notebook",
     "https://notebook.google.com/?icid=NotebookLM_A_B_test_google_oo_website_AB_test_main_cta_variant_A"},
    {"ChatGPT", "https://chatgpt.com/"},
    {"Claude", "https://claude.ai/"},
    {"Gemini", "https://gemini.google.com/"},
    {"DeepSeek", "https://chat.deepseek.com/"},
    {"Kimi", "https://kimi.moonshot.cn/"},
    {"Doubao", "https://www.doubao.com/chat/"},
    {"Grok", "https://grok.x.ai/"},
    {"Perplexity", "https://www.perplexity.ai/"},
    {"Microsoft Copilot", "https://copilot.microsoft.com/"},
    {"X", "https://x.com/"},
    {"Reddit", "https://www.reddit.com/"},
    {"YouTube Music", "https://music.youtube.com/"},
    {"Apple Podcasts", "https://podcasts.apple.com/"},
};

struct WebBookmark {
    Str title;
    Str url;
    bool pinned = false;
};

Vec<WebBookmark> gBookmarks;
Str gLastUrl;
int gCdpPort = kDefaultCdpPort;
WNDPROC gWebPanelBoxWndProc = nullptr;

int FindBookmarkByUrl(Str url);
void SaveBookmarks();
void SaveWebPanelTabs(MainWindow* win);
void LoadWebPanelTabs(MainWindow* win);
void RememberPdfActiveTab(MainWindow* win);
void RestorePdfActiveTab(MainWindow* win);
void CreateNewWebPanelTab(MainWindow* win, Str url, Str title, Str forcedId = {});
void ActivateWebPanelTabByIndex(MainWindow* win, int idx, bool rememberPdf);
void CloseWebPanelTabAt(MainWindow* win, int idx);
void CloseAllWebPanelTabs(MainWindow* win);
void RestorePdfNotebookLmTab(MainWindow* win);
void ShowTabsMenu(MainWindow* win);
void UpdateWebPanelCpuSample();
TempStr WebPanelResourceStatsTemp();
void RebuildPinStrip(MainWindow* win);
void EnsureWebPanelWebView(MainWindow* win);
void RestoreBookAiBindings(MainWindow* win, i64 bookId, LibraryBookKind kind);
void DeleteBookmarkAt(MainWindow* win, int idx);
void ActivateWebPanelTab(MainWindow* win, Str url);
void ShowActiveWebPanelTab(MainWindow* win);
WebviewWnd* CreateWebPanelTabWebView(MainWindow* win, Str url);
bool OnWebNavStarting(void* ctx, Str url, bool newWindow);
bool OnWebBrowserNavStarting(void* ctx, Str url, bool newWindow);
void OnWebNavCompleted(void* ctx, Str url, bool success);
void OnWebSourceChanged(void* ctx, WebviewWnd* sender, Str url);
void OnWebDocumentTitleChanged(void* ctx, WebviewWnd* sender, Str title);
void OnWebPanelJsNotify(void* ctx, Str method, Str paramsJson);
int FindWebPanelTabByWebView(MainWindow* win, WebviewWnd* wv);
void SyncWebPanelTabFromWebView(MainWindow* win, WebviewWnd* wv, Str url, Str title);
TempStr EscapeJsonTemp(Str s);
void EnsureBrowserExtensionsLayout();
static void SyncAndMaterializeOverlays();
TempStr BrowserExtensionsOverlayDirTemp();
void ActivateWebBrowserTabByIndex(MainWindow* win, int idx, bool remember);

TempStr WebPanelDataDirTemp() {
    // Synced / portable small state: tabs, bookmarks, bridge JSON pointer.
    return GetPathInAppDataDirTemp(StrL("WebPanel"));
}

TempStr WebPanelLocalDataDirTemp() {
    // Machine-local heavy state: WebView2 profiles, jobs, favicon cache.
    return GetPathInLocalSumatraDataDirTemp(StrL("WebPanel"));
}

static void MigrateWebPanelFile(Str fromRel, Str toRel) {
    TempStr root = WebPanelDataDirTemp();
    TempStr from = path::JoinTemp(root, fromRel);
    TempStr to = path::JoinTemp(root, toRel);
    if (!file::Exists(from) || file::Exists(to)) {
        return;
    }
    dir::CreateAll(path::GetDirTemp(to));
    file::Copy(to, from, false);
    // Keep the old file as a safety copy until the next cleanup; prefer new path.
}

static void MigrateWebPanelDir(Str fromRel, Str toRel) {
    TempStr root = WebPanelDataDirTemp();
    TempStr from = path::JoinTemp(root, fromRel);
    TempStr to = path::JoinTemp(root, toRel);
    if (!dir::Exists(from) || dir::Exists(to)) {
        return;
    }
    dir::CreateAll(path::GetDirTemp(to));
    // Best-effort rename; if locked, leave old path (readers still fall back below).
    MoveFileExW(CWStrTemp(from), CWStrTemp(to), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
}

// Move a heavy subtree from synced WebPanel → local WebPanel (OneDrive offload).
static bool DirHasAnyFile(Str dir, int depthLeft = 4) {
    if (!dir || !dir::Exists(dir) || depthLeft < 0) {
        return false;
    }
    WIN32_FIND_DATAW fd = {};
    TempStr pat = path::JoinTemp(dir, StrL("*"));
    HANDLE h = FindFirstFileW(CWStrTemp(pat), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
            continue;
        }
        TempStr child = path::JoinTemp(dir, ToUtf8Temp(fd.cFileName));
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            found = true;
            break;
        }
        if (DirHasAnyFile(child, depthLeft - 1)) {
            found = true;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static i64 DirByteSizeApprox(Str dir, int depthLeft = 6) {
    if (!dir || !dir::Exists(dir) || depthLeft < 0) {
        return 0;
    }
    i64 total = 0;
    WIN32_FIND_DATAW fd = {};
    TempStr pat = path::JoinTemp(dir, StrL("*"));
    HANDLE h = FindFirstFileW(CWStrTemp(pat), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
            continue;
        }
        TempStr child = path::JoinTemp(dir, ToUtf8Temp(fd.cFileName));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += DirByteSizeApprox(child, depthLeft - 1);
        } else {
            ULARGE_INTEGER li;
            li.LowPart = fd.nFileSizeLow;
            li.HighPart = fd.nFileSizeHigh;
            total += (i64)li.QuadPart;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static void MigrateHeavyDirToLocal(Str rel) {
    TempStr from = path::JoinTemp(WebPanelDataDirTemp(), rel);
    TempStr to = path::JoinTemp(WebPanelLocalDataDirTemp(), rel);
    if (!dir::Exists(from)) {
        return;
    }
    dir::CreateAll(path::GetDirTemp(to));
    if (!dir::Exists(to)) {
        BOOL ok = MoveFileExW(CWStrTemp(from), CWStrTemp(to), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
        logf("MigrateHeavyDirToLocal: '%s' → '%s' ok=%d\n", from, to, (int)ok);
        return;
    }
    if (DirHasAnyFile(to) && !DirHasAnyFile(from)) {
        return;
    }
    // Dest may be empty CreateAll stubs or a tiny fresh WebView2 profile created
    // before migration ran — replace when synced tree is clearly larger.
    WIN32_FIND_DATAW fd = {};
    TempStr pat = path::JoinTemp(from, StrL("*"));
    HANDLE h = FindFirstFileW(CWStrTemp(pat), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    int moved = 0;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
            continue;
        }
        TempStr childFrom = path::JoinTemp(from, ToUtf8Temp(fd.cFileName));
        TempStr childTo = path::JoinTemp(to, ToUtf8Temp(fd.cFileName));
        i64 fromSize = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DirByteSizeApprox(childFrom)
                       : ((i64)fd.nFileSizeHigh << 32) | (i64)fd.nFileSizeLow;
        i64 toSize = 0;
        if (file::Exists(childTo) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            toSize = file::GetSize(childTo);
        } else if (dir::Exists(childTo)) {
            toSize = DirByteSizeApprox(childTo);
        }
        // Keep local when it already holds equal/larger data (active machine profile).
        if (toSize > 0 && fromSize <= toSize) {
            continue;
        }
        if (dir::Exists(childTo)) {
            dir::RemoveAll(childTo);
        } else if (file::Exists(childTo)) {
            file::Delete(childTo);
        }
        BOOL ok = MoveFileExW(CWStrTemp(childFrom), CWStrTemp(childTo),
                              MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
        if (ok) {
            moved++;
        }
        logf("MigrateHeavyDirToLocal child: '%s' → '%s' from=%lld toWas=%lld ok=%d\n", childFrom,
             childTo, fromSize, toSize, (int)ok);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (moved > 0 && !DirHasAnyFile(from)) {
        dir::RemoveAll(from);
    } else if (DirHasAnyFile(to) && DirByteSizeApprox(from) > 0 &&
               DirByteSizeApprox(from) <= DirByteSizeApprox(to)) {
        // Local already holds equal/larger copy — drop leftover synced tree.
        dir::RemoveAll(from);
        logf("MigrateHeavyDirToLocal: removed leftover sync '%s'\n", from);
    }
    logf("MigrateHeavyDirToLocal: '%s' → '%s' movedChildren=%d\n", from, to, moved);
}

void EnsureWebPanelDataLayout() {
    TempStr syncRoot = WebPanelDataDirTemp();
    TempStr localRoot = WebPanelLocalDataDirTemp();
    dir::CreateAll(path::JoinTemp(syncRoot, StrL("tabs")));
    dir::CreateAll(path::JoinTemp(syncRoot, StrL("bridge")));
    // Called before every small state write (several per library click); the
    // migrations and README refreshes below cost ~50 ms each time.
    static bool layoutDone = false;
    if (layoutDone) {
        return;
    }
    layoutDone = true;
    // Legacy flat layout → structured under sync root (idempotent).
    MigrateWebPanelFile(StrL("tabs.json"), StrL("tabs\\index.json"));
    MigrateWebPanelFile(StrL("pdf-tabs.json"), StrL("tabs\\pdf-map.json"));
    MigrateWebPanelFile(StrL("web-bridge.json"), StrL("bridge\\web-bridge.json"));
    MigrateWebPanelDir(StrL("favicons"), StrL("cache\\favicons"));
    // Rename legacy profile folder names while still on sync root.
    MigrateWebPanelDir(StrL("WebView2"), StrL("profile\\Browser-AIChat"));
    MigrateWebPanelDir(StrL("profile\\WebView2"), StrL("profile\\Browser-AIChat"));
    MigrateWebPanelDir(StrL("profile\\WebView2-Browser"), StrL("profile\\Browser-Library"));
    // Offload heavy trees BEFORE creating empty local stubs (stubs would block MoveFile).
    MigrateHeavyDirToLocal(StrL("profile"));
    MigrateHeavyDirToLocal(StrL("cache"));
    MigrateHeavyDirToLocal(StrL("jobs"));
    // Ensure canonical dirs exist after migration.
    dir::CreateAll(path::JoinTemp(localRoot, StrL("profile\\Browser-AIChat")));
    dir::CreateAll(path::JoinTemp(localRoot, StrL("profile\\Browser-Library")));
    dir::CreateAll(path::JoinTemp(localRoot, StrL("cache\\favicons")));
    dir::CreateAll(path::JoinTemp(localRoot, StrL("jobs\\pending")));
    dir::CreateAll(path::JoinTemp(localRoot, StrL("jobs\\done")));
    dir::CreateAll(path::JoinTemp(localRoot, StrL("jobs\\failed")));
    MigrateWebPanelFile(StrL("bridge.log"), StrL("bridge\\bridge.log"));
    // Prefer local bridge.log; move if still under sync.
    {
        TempStr syncLog = path::JoinTemp(syncRoot, StrL("bridge\\bridge.log"));
        TempStr localLog = path::JoinTemp(localRoot, StrL("bridge\\bridge.log"));
        if (file::Exists(syncLog) && !file::Exists(localLog)) {
            dir::CreateAll(path::GetDirTemp(localLog));
            MoveFileExW(CWStrTemp(syncLog), CWStrTemp(localLog), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
        }
    }
    // Human-readable layout guide (always refresh).
    file::WriteFile(
        path::JoinTemp(syncRoot, StrL("README.txt")),
        StrL("SumatraPDF WebPanel — SYNCED small state (OneDrive / portable)\r\n"
             "\r\n"
             "tabs\\             tab session + per-book active-tab map\r\n"
             "  index.json       AI panel open tabs / active tab id\r\n"
             "  web-index.json   Library (center) Web open tabs\r\n"
             "  pdf-map.json     bookId → AI + browser tab bindings + aiOpen\r\n"
             "bridge\\           web-bridge.json (CDP ports; tiny)\r\n"
             "bookmarks.txt      AI bookmark list\r\n"
             "\r\n"
             "HEAVY state lives under %%LOCALAPPDATA%%\\SumatraPDF\\WebPanel\\\r\n"
             "  (profiles / cache / jobs) — not synced, re-login once per machine.\r\n"
             "\r\n"
             "Extensions: %%AppData-or-OneDrive%%\\SumatraPDF\\extensions\\\r\n"
             "Scripts:    …\\SumatraPDF\\scripts\\webview\\  (or junction to repo)\r\n"
             "GitHub: https://github.com/tingaidehua/sumatrapdf-browsers-agents\r\n"));
    file::WriteFile(
        path::JoinTemp(localRoot, StrL("README.txt")),
        StrL("SumatraPDF WebPanel — LOCAL heavy state (%%LOCALAPPDATA%%\\SumatraPDF\\WebPanel)\r\n"
             "\r\n"
             "profile\\Browser-AIChat\\   AI WebView2 user data (CDP 9224)\r\n"
             "profile\\Browser-Library\\  center Web user data (CDP 9225)\r\n"
             "cache\\favicons\\\r\n"
             "jobs\\pending|done|failed\\\r\n"
             "bridge\\bridge.log\r\n"
             "\r\n"
             "Do not move this folder into OneDrive.\r\n"));
    EnsureBrowserExtensionsLayout();
    EnsureScriptsWebviewLayout();
}

static TempStr PreferLocalOrSyncRelTemp(Str rel, Str legacyRel, bool acceptEmptyLocal) {
    TempStr local = path::JoinTemp(WebPanelLocalDataDirTemp(), rel);
    if (file::Exists(local)) {
        return local;
    }
    if (dir::Exists(local) && (acceptEmptyLocal || DirHasAnyFile(local))) {
        return local;
    }
    TempStr sync = path::JoinTemp(WebPanelDataDirTemp(), rel);
    if (file::Exists(sync) || (dir::Exists(sync) && DirHasAnyFile(sync))) {
        return sync;
    }
    if (dir::Exists(sync) && acceptEmptyLocal) {
        return sync;
    }
    if (legacyRel) {
        TempStr leg = path::JoinTemp(WebPanelDataDirTemp(), legacyRel);
        if (file::Exists(leg) || (dir::Exists(leg) && DirHasAnyFile(leg))) {
            return leg;
        }
    }
    return local;
}

static TempStr PreferNewOrLegacyTemp(Str newRel, Str legacyRel) {
    // Sync-root prefer helper (tabs / bridge JSON stay synced).
    TempStr root = WebPanelDataDirTemp();
    TempStr neu = path::JoinTemp(root, newRel);
    if (file::Exists(neu) || dir::Exists(neu)) {
        return neu;
    }
    TempStr old = path::JoinTemp(root, legacyRel);
    if (file::Exists(old) || dir::Exists(old)) {
        return old;
    }
    return neu;
}

TempStr BookmarksPathTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("bookmarks.txt"));
}

TempStr TabsPathCanonicalTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("tabs\\index.json"));
}

TempStr TabsPathTemp() {
    return PreferNewOrLegacyTemp(StrL("tabs\\index.json"), StrL("tabs.json"));
}

TempStr PdfTabsMapPathCanonicalTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("tabs\\pdf-map.json"));
}

TempStr PdfTabsMapPathTemp() {
    return PreferNewOrLegacyTemp(StrL("tabs\\pdf-map.json"), StrL("pdf-tabs.json"));
}

TempStr BridgePathCanonicalTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("bridge\\web-bridge.json"));
}

TempStr BridgePathTemp() {
    return PreferNewOrLegacyTemp(StrL("bridge\\web-bridge.json"), StrL("web-bridge.json"));
}

TempStr WebViewProfileDirTemp() {
    // AI panel — Browser-AIChat (CDP 9224). Prefer LOCAL; fall back to synced legacy.
    return PreferLocalOrSyncRelTemp(StrL("profile\\Browser-AIChat"), StrL("profile\\WebView2"), false);
}

TempStr WebViewBrowserProfileDirTemp() {
    // Center Library Web — Browser-Library (CDP 9225).
    return PreferLocalOrSyncRelTemp(StrL("profile\\Browser-Library"), StrL("profile\\WebView2-Browser"), false);
}

TempStr BrowserExtensionsRootDirTemp() {
    return path::JoinTemp(GetAppDataDirTemp(), StrL("extensions"));
}

TempStr BrowserExtensionsInstalledDirTemp() {
    return path::JoinTemp(BrowserExtensionsRootDirTemp(), StrL("installed"));
}

TempStr BrowserExtensionsOverlayDirTemp() {
    // Chrome payloads — machine-local, never git / never OneDrive.
    return GetPathInLocalSumatraDataDirTemp(StrL("extensions\\overlay"));
}

void EnsureBrowserExtensionsLayout() {
    TempStr root = BrowserExtensionsRootDirTemp();
    TempStr installed = BrowserExtensionsInstalledDirTemp();
    dir::CreateAll(installed);
    TempStr readme = path::JoinTemp(root, StrL("README.txt"));
    // Always refresh guide so repo rename / layout notes stay current.
    file::WriteFile(
        readme,
        StrL("SumatraPDF browsers / agents — extensions\r\n"
             "\r\n"
             "installed\\<id>\\plugin.json   host plugin (native / Playwright action)\r\n"
             "overlays\\<id>.json           overlay descriptor (tiny; tracked in git)\r\n"
             "\r\n"
             "Chrome payloads (NOT in git, NOT on OneDrive):\r\n"
             "  %%LOCALAPPDATA%%\\SumatraPDF\\extensions\\overlay\\<id>\\\r\n"
             "  Copied from Chrome Extensions\\<chromeId> on launch (strip _metadata).\r\n"
             "\r\n"
             "plugin.json \"browser\": Browser-AIChat | Browser-Library\r\n"
             "\r\n"
             "Built-in host plugins (Browser-AIChat only):\r\n"
             "  notebooklm-add / notebooklm-focus-pdf\r\n"
             "Overlay: Trancy (Browser-Library) — right-click 沉浸式翻译 + options\r\n"
             "GitHub: https://github.com/tingaidehua/sumatrapdf-browsers-agents\r\n"));

    // Seed / refresh built-in host plugins (plugin.json only — not Chrome CRX).
    // browser: Browser-AIChat | Browser-Library — which WebView2 toolbar may run them.
    auto seedHost = [&](Str id, Str name, Str action, Str description, Str browser) {
        TempStr dir = path::JoinTemp(installed, id);
        dir::CreateAll(dir);
        TempStr jsonPath = path::JoinTemp(dir, StrL("plugin.json"));
        TempStr body =
            fmt("{\n"
                "  \"id\": %s,\n"
                "  \"name\": %s,\n"
                "  \"kind\": \"host\",\n"
                "  \"browser\": %s,\n"
                "  \"action\": %s,\n"
                "  \"description\": %s\n"
                "}\n",
                EscapeJsonTemp(id), EscapeJsonTemp(name), EscapeJsonTemp(browser), EscapeJsonTemp(action),
                EscapeJsonTemp(description));
        file::WriteFile(jsonPath, body);
    };
    seedHost(StrL("notebooklm-add"), StrL("添加到 NotebookLM"), StrL("notebooklm.add"),
             StrL("将中间栏当前 PDF 或网页加入 NotebookLM（Playwright / CDP 9224）"),
             StrL("Browser-AIChat"));
    seedHost(StrL("notebooklm-focus-pdf"), StrL("仅与当前 PDF 对话"), StrL("notebooklm.focus"),
             StrL("按图书馆 NotebookLM 记录，仅选中中间栏当前 PDF/网页来源并打开对话"),
             StrL("Browser-AIChat"));

    SyncAndMaterializeOverlays();
}

TempStr FaviconsDirTemp() {
    return PreferLocalOrSyncRelTemp(StrL("cache\\favicons"), StrL("favicons"), true);
}

TempStr WebPanelJobsPendingTemp() {
    return PreferLocalOrSyncRelTemp(StrL("jobs\\pending"), {}, true);
}

TempStr WebPanelJobsDoneTemp() {
    return PreferLocalOrSyncRelTemp(StrL("jobs\\done"), {}, true);
}

TempStr WebPanelJobsFailedTemp() {
    return PreferLocalOrSyncRelTemp(StrL("jobs\\failed"), {}, true);
}

// host for https://a.b/c → a.b
TempStr HostFromUrlTemp(Str url) {
    if (!url) {
        return {};
    }
    Str rest = url;
    if (str::StartsWithI(rest, StrL("https://"))) {
        rest = Str(rest.s + 8, rest.len - 8);
    } else if (str::StartsWithI(rest, StrL("http://"))) {
        rest = Str(rest.s + 7, rest.len - 7);
    } else {
        return {};
    }
    int end = 0;
    while (end < rest.len && rest.s[end] != '/' && rest.s[end] != '?' && rest.s[end] != '#' && rest.s[end] != ':') {
        end++;
    }
    if (end <= 0) {
        return {};
    }
    return str::DupTemp(Str(rest.s, end));
}

TempStr FaviconCachePathTemp(Str host) {
    if (!host) {
        return {};
    }
    str::Builder name;
    for (int i = 0; i < host.len; i++) {
        char c = host.s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-') {
            name.AppendChar(c);
        } else {
            name.AppendChar('_');
        }
    }
    name.Append(StrL(".png"));
    return path::JoinTemp(FaviconsDirTemp(), ToStrTemp(name));
}

// browser-style favicon from disk cache only (never network on UI thread).
// Sized to fit the sidebar header (same band as Library / ToC labels).
constexpr int kPinIconPx = 14;

Pixmap* LoadFaviconPixmap(Str url) {
    int sz = DpiScale(kPinIconPx);
    TempStr host = HostFromUrlTemp(url);
    if (host) {
        TempStr cachePath = FaviconCachePathTemp(host);
        if (cachePath && file::Exists(cachePath)) {
            Str data = file::ReadFile(cachePath);
            if (data) {
                Pixmap* px = PixmapFromData(data);
                str::Free(data);
                if (px) {
                    // VirtIconButton blits at native pixmap size — keep a small copy
                    if (px->width == sz && px->height == sz) {
                        return px;
                    }
                    // draw into an ideal-sized pixmap via VirtImage-style fit:
                    // store original; pin button scales at paint time
                    return px;
                }
            }
        }
    }
    return GetCachedPixmapForSvg(gIconChat, sz, sz);
}

void FetchMissingFavicons(Vec<Str>* urls) {
    if (!urls) {
        return;
    }
    dir::CreateAll(FaviconsDirTemp());
    for (Str& url : *urls) {
        TempStr host = HostFromUrlTemp(url);
        if (!host) {
            continue;
        }
        TempStr cachePath = FaviconCachePathTemp(host);
        if (!cachePath || file::Exists(cachePath)) {
            continue;
        }
        TempStr favUrl = fmt("https://www.google.com/s2/favicons?domain=%s&sz=32", host);
        HttpRsp rsp;
        if (HttpGet(favUrl, &rsp) && IsHttpRspOk(&rsp) && len(rsp.data) > 0) {
            file::WriteFile(cachePath, ToStr(rsp.data));
        }
    }
}

struct FaviconJob {
    MainWindow* win = nullptr;
    Vec<Str>* urls = nullptr;
};

void OnFaviconsFetched(FaviconJob* job) {
    if (!job) {
        return;
    }
    MainWindow* win = job->win;
    if (job->urls) {
        for (Str& u : *job->urls) {
            str::Free(u);
        }
        delete job->urls;
    }
    delete job;
    if (!IsMainWindowValid(win) || !win->hwndWebPanelBox) {
        return;
    }
    RebuildPinStrip(win);
    RelayoutWebPanel(win);
}

void FaviconFetchThread(FaviconJob* job) {
    if (!job) {
        return;
    }
    FetchMissingFavicons(job->urls);
    uitask::Post(MkFunc0(OnFaviconsFetched, job), "WebPanelFaviconsDone");
}

void ScheduleFaviconPrefetch(MainWindow* win) {
    if (!win) {
        return;
    }
    auto* urls = new Vec<Str>();
    for (WebBookmark& b : gBookmarks) {
        if (b.pinned && b.url) {
            TempStr host = HostFromUrlTemp(b.url);
            TempStr cachePath = host ? FaviconCachePathTemp(host) : TempStr{};
            if (cachePath && file::Exists(cachePath)) {
                continue;
            }
            urls->Append(str::Dup(b.url));
        }
    }
    if (len(*urls) == 0) {
        delete urls;
        return;
    }
    auto* job = new FaviconJob{win, urls};
    RunAsync(MkFunc0(FaviconFetchThread, job), "WebPanelFavicons");
}

void DeferredEnsureWebPanelWebView(MainWindow* win) {
    if (!IsMainWindowValid(win) || !win->uiState.webPanelVisible) {
        return;
    }
    EnsureWebPanelWebView(win);
    ScheduleUiUpdate(win);
    ScheduleFaviconPrefetch(win);
}

TempStr ClipboardTextTemp() {
    if (!OpenClipboard(nullptr)) {
        return {};
    }
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    TempStr out = {};
    if (h) {
        auto* w = (WCHAR*)GlobalLock(h);
        if (w) {
            out = ToUtf8Temp(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out;
}

void FreeBookmarks() {
    for (WebBookmark& b : gBookmarks) {
        str::Free(b.title);
        str::Free(b.url);
    }
    gBookmarks.Reset();
}

void EnsureDefaultBookmarks() {
    bool changed = false;
    for (const DefaultBookmark& def : kDefaultBookmarks) {
        if (FindBookmarkByUrl(def.url) >= 0) {
            continue;
        }
        WebBookmark b;
        b.title = str::Dup(def.title);
        b.url = str::Dup(def.url);
        b.pinned = true;
        gBookmarks.Append(b);
        changed = true;
    }
    if (changed) {
        SaveBookmarks();
    }
}

void LoadBookmarks() {
    FreeBookmarks();
    str::Free(gLastUrl);
    gLastUrl = {};
    gCdpPort = kDefaultCdpPort;

    TempStr path = BookmarksPathTemp();
    Str data = file::ReadFile(path);
    if (!data) {
        EnsureDefaultBookmarks();
        return;
    }
    StrVec lines;
    Split(&lines, data, StrL("\n"), true);
    str::Free(data);
    for (Str line : lines) {
        while (line.len > 0 && (line.s[line.len - 1] == '\r' || line.s[line.len - 1] == ' ')) {
            line.len--;
        }
        if (line.len == 0 || line.s[0] == '#') {
            continue;
        }
        if (str::StartsWith(line, StrL("last="))) {
            TempStr last = str::DupTemp(Str(line.s + 5, line.len - 5));
            if (last && !str::EqI(last, StrL("about:blank")) && !str::StartsWithI(last, StrL("about:"))) {
                gLastUrl = str::Dup(last);
            }
            continue;
        }
        if (str::StartsWith(line, StrL("cdp="))) {
            int port = 0;
            if (str::Parse(Str(line.s + 4, line.len - 4), "%d", &port).s && port > 0) {
                // Migrate pre-9224 profiles; keep a single well-known port for the AI bridge.
                gCdpPort = (port == 9223) ? kDefaultCdpPort : port;
            }
            continue;
        }
        StrVec parts;
        Split(&parts, line, StrL("|"), false);
        if (len(parts) < 2) {
            continue;
        }
        WebBookmark b;
        b.title = str::Dup(parts[0]);
        b.url = str::Dup(parts[1]);
        b.pinned = len(parts) >= 3 && str::Eq(parts[2], StrL("1"));
        if (b.title && b.url) {
            gBookmarks.Append(b);
        } else {
            str::Free(b.title);
            str::Free(b.url);
        }
    }
    EnsureDefaultBookmarks();
}

void SaveBookmarks() {
    TempStr dir = WebPanelDataDirTemp();
    dir::CreateAll(dir);
    str::Builder sb;
    sb.Append("# WebPanel bookmarks: title|url|pinned\n");
    if (gLastUrl) {
        sb.Append(fmt("last=%s\n", gLastUrl));
    }
    sb.Append(fmt("cdp=%d\n", gCdpPort));
    for (WebBookmark& b : gBookmarks) {
        sb.Append(fmt("%s|%s|%d\n", b.title ? b.title : "", b.url ? b.url : "", b.pinned ? 1 : 0));
    }
    file::WriteFile(BookmarksPathTemp(), ToStr(sb));
}

static i64 DirSizeBytes(Str dir) {
    if (!dir || !dir::Exists(dir)) {
        return 0;
    }
    i64 total = 0;
    DirIter di(dir);
    di.recurse = true;
    di.includeFiles = true;
    di.includeDirs = false;
    for (DirIterEntry* e : di) {
        if (e && e->isFile) {
            total += e->size > 0 ? e->size : 0;
        }
    }
    return total;
}

static i64 ProcessWorkingSetBytes(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        return 0;
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    i64 bytes = 0;
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
        bytes = (i64)pmc.WorkingSetSize;
    }
    CloseHandle(h);
    return bytes;
}

static i64 WebViewChildWorkingSetBytes() {
    i64 total = 0;
    DWORD myPid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID == myPid) {
                TempWStr name = pe.szExeFile;
                TempStr nameU = ToUtf8Temp(name);
                if (nameU && (str::ContainsI(nameU, StrL("msedgewebview2")) ||
                              str::ContainsI(nameU, StrL("webview2")))) {
                    total += ProcessWorkingSetBytes(pe.th32ProcessID);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return total;
}

static ULONGLONG FileTimeToU64(const FILETIME& ft) {
    return (((ULONGLONG)ft.dwHighDateTime) << 32) | (ULONGLONG)ft.dwLowDateTime;
}

static void AccumulateWebViewChildCpuTimes(ULONGLONG* kernelOut, ULONGLONG* userOut) {
    if (!kernelOut || !userOut) {
        return;
    }
    DWORD myPid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID != myPid) {
                continue;
            }
            TempStr nameU = ToUtf8Temp(pe.szExeFile);
            if (!nameU || !(str::ContainsI(nameU, StrL("msedgewebview2")) ||
                            str::ContainsI(nameU, StrL("webview2")))) {
                continue;
            }
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) {
                h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
            }
            if (!h) {
                continue;
            }
            FILETIME c{}, e{}, k{}, u{};
            if (GetProcessTimes(h, &c, &e, &k, &u)) {
                *kernelOut += FileTimeToU64(k);
                *userOut += FileTimeToU64(u);
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

struct WebPanelCpuSample {
    ULONGLONG sysIdle = 0;
    ULONGLONG sysKernel = 0;
    ULONGLONG sysUser = 0;
    ULONGLONG procKernel = 0;
    ULONGLONG procUser = 0;
    bool valid = false;
};

static WebPanelCpuSample gWebPanelCpuPrev{};
static double gWebPanelCpuPct = -1;

void UpdateWebPanelCpuSample() {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return;
    }
    FILETIME c{}, e{}, pk{}, pu{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu)) {
        return;
    }
    WebPanelCpuSample cur;
    cur.sysIdle = FileTimeToU64(idle);
    cur.sysKernel = FileTimeToU64(kernel);
    cur.sysUser = FileTimeToU64(user);
    cur.procKernel = FileTimeToU64(pk);
    cur.procUser = FileTimeToU64(pu);
    AccumulateWebViewChildCpuTimes(&cur.procKernel, &cur.procUser);
    cur.valid = true;
    if (gWebPanelCpuPrev.valid) {
        ULONGLONG dSys = (cur.sysKernel - gWebPanelCpuPrev.sysKernel) + (cur.sysUser - gWebPanelCpuPrev.sysUser);
        ULONGLONG dProc =
            (cur.procKernel - gWebPanelCpuPrev.procKernel) + (cur.procUser - gWebPanelCpuPrev.procUser);
        if (dSys > 0) {
            gWebPanelCpuPct = (100.0 * (double)dProc) / (double)dSys;
            if (gWebPanelCpuPct < 0) {
                gWebPanelCpuPct = 0;
            }
            if (gWebPanelCpuPct > 999) {
                gWebPanelCpuPct = 999;
            }
        }
    }
    gWebPanelCpuPrev = cur;
}

TempStr WebPanelResourceStatsTemp() {
    UpdateWebPanelCpuSample();
    i64 disk = DirSizeBytes(WebPanelDataDirTemp());
    i64 mem = ProcessWorkingSetBytes(GetCurrentProcessId()) + WebViewChildWorkingSetBytes();
    TempStr cpu = gWebPanelCpuPct < 0 ? StrL("—") : fmt("%.0f%%", gWebPanelCpuPct);
    return fmt(_TRA("调试 · CPU %s · 内存 %s · 磁盘 %s · Tab 现场保留").s, cpu,
               FormatFileSizeShortTransTemp(mem), FormatFileSizeShortTransTemp(disk));
}

TempStr NewWebPanelTabIdTemp() {
    return fmt("t%lld-%d", UnixTimeMsNow(), (int)(GetTickCount() & 0xffff));
}

TempStr TitleFromUrlTemp(Str url) {
    TempStr host = HostFromUrlTemp(url);
    return host ? host : (url ? str::DupTemp(url) : StrL("(blank)"));
}

void SaveWebPanelTabs(MainWindow* win) {
    if (!win) {
        return;
    }
    EnsureWebPanelDataLayout();
    str::Builder sb;
    sb.Append(StrL("{\n  \"activeId\": "));
    Str activeId = {};
    if (win->webPanelActiveTab >= 0 && win->webPanelActiveTab < len(win->webPanelTabs)) {
        activeId = win->webPanelTabs[win->webPanelActiveTab].id;
    }
    sb.Append(EscapeJsonTemp(activeId));
    sb.Append(StrL(",\n  \"tabs\": [\n"));
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        WebPanelTab& t = win->webPanelTabs[i];
        if (i > 0) {
            sb.Append(StrL(",\n"));
        }
        sb.Append(StrL("    {\"id\": "));
        sb.Append(EscapeJsonTemp(t.id));
        sb.Append(StrL(", \"url\": "));
        sb.Append(EscapeJsonTemp(t.url));
        sb.Append(StrL(", \"title\": "));
        sb.Append(EscapeJsonTemp(t.title ? t.title : TitleFromUrlTemp(t.url)));
        if (t.titleLocked) {
            sb.Append(StrL(", \"titleLocked\": true"));
        }
        sb.Append(StrL("}"));
    }
    sb.Append(StrL("\n  ]\n}\n"));
    file::WriteFile(TabsPathCanonicalTemp(), ToStr(sb));
}

void LoadWebPanelTabs(MainWindow* win) {
    if (!win) {
        return;
    }
    // Keep existing live WebViews if already loaded.
    if (len(win->webPanelTabs) > 0) {
        return;
    }
    Str data = file::ReadFile(TabsPathTemp());
    if (!data) {
        return;
    }
    struct St {
        MainWindow* win = nullptr;
        WebPanelTab cur{};
        bool inTab = false;
        Str activeId;
    } st;
    st.win = win;
    auto onVal = [](St* s, json::Value* v) {
        TempStr p = json::PathFormatTemp(v->path);
        if (!p) {
            return;
        }
        if (str::EqI(p, StrL("/activeId"))) {
            str::ReplaceWithCopy(&s->activeId, v->value);
            return;
        }
        if (!str::ContainsI(p, StrL("/tabs"))) {
            return;
        }
        if (str::EndsWithI(p, StrL("/id"))) {
            if (s->inTab && s->cur.url) {
                if (!s->cur.id) {
                    s->cur.id = str::Dup(NewWebPanelTabIdTemp());
                }
                if (!s->cur.title) {
                    s->cur.title = str::Dup(TitleFromUrlTemp(s->cur.url));
                }
                s->win->webPanelTabs.Append(s->cur);
                s->cur = {};
            }
            s->inTab = true;
            str::ReplaceWithCopy(&s->cur.id, v->value);
        } else if (str::EndsWithI(p, StrL("/url"))) {
            str::ReplaceWithCopy(&s->cur.url, v->value);
        } else if (str::EndsWithI(p, StrL("/title"))) {
            str::ReplaceWithCopy(&s->cur.title, v->value);
        } else if (str::EndsWithI(p, StrL("/titleLocked"))) {
            s->cur.titleLocked = v->value && (str::EqI(v->value, StrL("true")) || str::Eq(v->value, StrL("1")));
        }
    };
    json::Parse(data, MkFunc1<St, json::Value*>(onVal, &st));
    if (st.inTab && st.cur.url) {
        if (!st.cur.id) {
            st.cur.id = str::Dup(NewWebPanelTabIdTemp());
        }
        if (!st.cur.title) {
            st.cur.title = str::Dup(TitleFromUrlTemp(st.cur.url));
        }
        win->webPanelTabs.Append(st.cur);
        st.cur = {};
    }
    str::Free(st.cur.id);
    str::Free(st.cur.url);
    str::Free(st.cur.title);
    str::Free(data);
    win->webPanelActiveTab = -1;
    if (st.activeId) {
        for (int i = 0; i < len(win->webPanelTabs); i++) {
            if (str::Eq(win->webPanelTabs[i].id, st.activeId)) {
                win->webPanelActiveTab = i;
                break;
            }
        }
        str::Free(st.activeId);
    }
}

struct PdfTabBinding {
    Str bookKey; // decimal book id
    // AI panel: "当前" + NotebookLM (same for PDF and Web library entries)
    Str webTabId;
    Str webTabUrl;
    Str notebookTabId;
    Str notebookTabUrl;
    // Center Web browser surface (Web library entries only)
    Str browserTabId;
    Str browserTabUrl;
    // -1 = never set (treat as open, matching historical default); 0 = closed; 1 = open
    int aiOpen = -1;
};

static bool IsNotebookLmUrl(Str url) {
    if (!url) {
        return false;
    }
    return str::StartsWithI(url, StrL("https://notebook.google.com")) ||
           str::StartsWithI(url, StrL("http://notebook.google.com")) ||
           str::ContainsI(url, StrL("notebooklm.google")) ||
           str::ContainsI(url, StrL("notebook.google.com"));
}

static void FreePdfTabBinding(PdfTabBinding* b) {
    if (!b) {
        return;
    }
    str::Free(b->bookKey);
    str::Free(b->webTabId);
    str::Free(b->webTabUrl);
    str::Free(b->notebookTabId);
    str::Free(b->notebookTabUrl);
    str::Free(b->browserTabId);
    str::Free(b->browserTabUrl);
    *b = {};
}

static PdfTabBinding* FindPdfTabBinding(Vec<PdfTabBinding>* all, Str bookKey) {
    if (!all || !bookKey) {
        return nullptr;
    }
    for (PdfTabBinding& b : *all) {
        if (str::Eq(b.bookKey, bookKey)) {
            return &b;
        }
    }
    return nullptr;
}

static Vec<PdfTabBinding> LoadAllPdfTabBindings() {
    Vec<PdfTabBinding> all;
    Str data = file::ReadFile(PdfTabsMapPathTemp());
    if (!data) {
        return all;
    }
    struct St {
        Vec<PdfTabBinding>* all = nullptr;
    } st;
    st.all = &all;
    auto onVal = [](St* s, json::Value* v) {
        TempStr p = json::PathFormatTemp(v->path);
        if (!p || p.s[0] != '/') {
            return;
        }
        // /bookKey or /bookKey/field
        Str rest = Str(p.s + 1, p.len - 1);
        int slash = str::IndexOfChar(rest, '/');
        Str bookKey = slash < 0 ? rest : Str(rest.s, slash);
        Str field = slash < 0 ? Str{} : Str(rest.s + slash + 1, rest.len - slash - 1);
        if (!bookKey) {
            return;
        }
        PdfTabBinding* b = FindPdfTabBinding(s->all, bookKey);
        if (!b) {
            PdfTabBinding nb{};
            nb.bookKey = str::Dup(bookKey);
            s->all->Append(nb);
            b = &s->all->Last();
        }
        if (v->type != json::Type::String || !v->value) {
            return;
        }
        if (!field) {
            // Legacy flat "bookId": "tabId"
            if (!b->webTabId) {
                b->webTabId = str::Dup(v->value);
            }
            return;
        }
        if (str::Eq(field, StrL("webTabId")) || str::Eq(field, StrL("tabId"))) {
            str::ReplaceWithCopy(&b->webTabId, v->value);
        } else if (str::Eq(field, StrL("webTabUrl")) || str::Eq(field, StrL("tabUrl"))) {
            str::ReplaceWithCopy(&b->webTabUrl, v->value);
        } else if (str::Eq(field, StrL("notebookTabId"))) {
            str::ReplaceWithCopy(&b->notebookTabId, v->value);
        } else if (str::Eq(field, StrL("notebookTabUrl"))) {
            str::ReplaceWithCopy(&b->notebookTabUrl, v->value);
        } else if (str::Eq(field, StrL("browserTabId"))) {
            str::ReplaceWithCopy(&b->browserTabId, v->value);
        } else if (str::Eq(field, StrL("browserTabUrl"))) {
            str::ReplaceWithCopy(&b->browserTabUrl, v->value);
        } else if (str::Eq(field, StrL("aiOpen"))) {
            if (v->value && (str::Eq(v->value, StrL("1")) || str::EqI(v->value, StrL("true")))) {
                b->aiOpen = 1;
            } else if (v->value && (str::Eq(v->value, StrL("0")) || str::EqI(v->value, StrL("false")))) {
                b->aiOpen = 0;
            }
        }
    };
    json::Parse(data, MkFunc1<St, json::Value*>(onVal, &st));
    str::Free(data);
    return all;
}

static void SaveAllPdfTabBindings(Vec<PdfTabBinding>& all) {
    EnsureWebPanelDataLayout();
    str::Builder out;
    out.Append(StrL("{\n"));
    bool first = true;
    for (PdfTabBinding& b : all) {
        if (!b.bookKey) {
            continue;
        }
        if (!b.webTabId && !b.webTabUrl && !b.notebookTabId && !b.notebookTabUrl && !b.browserTabId &&
            !b.browserTabUrl && b.aiOpen < 0) {
            continue;
        }
        if (!first) {
            out.Append(StrL(",\n"));
        }
        first = false;
        out.Append(fmt("  %s: {\n", EscapeJsonTemp(b.bookKey)));
        bool f2 = true;
        auto field = [&](Str name, Str val) {
            if (!val) {
                return;
            }
            if (!f2) {
                out.Append(StrL(",\n"));
            }
            f2 = false;
            out.Append(fmt("    %s: %s", EscapeJsonTemp(name), EscapeJsonTemp(val)));
        };
        field(StrL("webTabId"), b.webTabId);
        field(StrL("webTabUrl"), b.webTabUrl);
        field(StrL("notebookTabId"), b.notebookTabId);
        field(StrL("notebookTabUrl"), b.notebookTabUrl);
        field(StrL("browserTabId"), b.browserTabId);
        field(StrL("browserTabUrl"), b.browserTabUrl);
        if (b.aiOpen >= 0) {
            field(StrL("aiOpen"), b.aiOpen > 0 ? StrL("1") : StrL("0"));
        }
        out.Append(StrL("\n  }"));
    }
    out.Append(StrL("\n}\n"));
    file::WriteFile(PdfTabsMapPathCanonicalTemp(), ToStr(out));
}

static void FreeAllPdfTabBindings(Vec<PdfTabBinding>& all) {
    for (PdfTabBinding& b : all) {
        FreePdfTabBinding(&b);
    }
    all.Reset();
}

static PdfTabBinding LoadPdfTabBindingForBook(i64 bookId) {
    PdfTabBinding empty{};
    if (bookId <= 0) {
        return empty;
    }
    TempStr key = fmt("%lld", bookId);
    Vec<PdfTabBinding> all = LoadAllPdfTabBindings();
    PdfTabBinding* found = FindPdfTabBinding(&all, key);
    PdfTabBinding out{};
    if (found) {
        out.bookKey = str::Dup(found->bookKey);
        out.webTabId = str::Dup(found->webTabId);
        out.webTabUrl = str::Dup(found->webTabUrl);
        out.notebookTabId = str::Dup(found->notebookTabId);
        out.notebookTabUrl = str::Dup(found->notebookTabUrl);
        out.browserTabId = str::Dup(found->browserTabId);
        out.browserTabUrl = str::Dup(found->browserTabUrl);
        out.aiOpen = found->aiOpen;
    }
    FreeAllPdfTabBindings(all);
    return out;
}

static void UpsertPdfTabBinding(i64 bookId, Str webTabId, Str webTabUrl, Str notebookTabId, Str notebookTabUrl,
                                bool setWeb, bool setNotebook, bool clearIds, bool clearUrls,
                                Str browserTabId = {}, Str browserTabUrl = {}, bool setBrowser = false) {
    if (bookId <= 0) {
        return;
    }
    TempStr key = fmt("%lld", bookId);
    Vec<PdfTabBinding> all = LoadAllPdfTabBindings();
    PdfTabBinding* b = FindPdfTabBinding(&all, key);
    if (!b) {
        PdfTabBinding nb{};
        nb.bookKey = str::Dup(key);
        all.Append(nb);
        b = &all.Last();
    }
    if (clearIds) {
        str::Free(b->webTabId);
        str::Free(b->notebookTabId);
        str::Free(b->browserTabId);
        b->webTabId = {};
        b->notebookTabId = {};
        b->browserTabId = {};
    }
    if (clearUrls) {
        str::Free(b->webTabUrl);
        str::Free(b->notebookTabUrl);
        str::Free(b->browserTabUrl);
        b->webTabUrl = {};
        b->notebookTabUrl = {};
        b->browserTabUrl = {};
    }
    if (setWeb) {
        if (webTabId) {
            str::ReplaceWithCopy(&b->webTabId, webTabId);
        }
        if (webTabUrl) {
            str::ReplaceWithCopy(&b->webTabUrl, webTabUrl);
        }
    }
    if (setNotebook) {
        if (notebookTabId) {
            str::ReplaceWithCopy(&b->notebookTabId, notebookTabId);
        }
        if (notebookTabUrl) {
            str::ReplaceWithCopy(&b->notebookTabUrl, notebookTabUrl);
        }
    }
    if (setBrowser) {
        if (browserTabId) {
            str::ReplaceWithCopy(&b->browserTabId, browserTabId);
        }
        if (browserTabUrl) {
            str::ReplaceWithCopy(&b->browserTabUrl, browserTabUrl);
        }
    }
    SaveAllPdfTabBindings(all);
    FreeAllPdfTabBindings(all);
}

static void SetBookAiPanelOpen(i64 bookId, bool open) {
    if (bookId <= 0) {
        return;
    }
    TempStr key = fmt("%lld", bookId);
    Vec<PdfTabBinding> all = LoadAllPdfTabBindings();
    PdfTabBinding* b = FindPdfTabBinding(&all, key);
    if (!b) {
        PdfTabBinding nb{};
        nb.bookKey = str::Dup(key);
        all.Append(nb);
        b = &all.Last();
    }
    b->aiOpen = open ? 1 : 0;
    SaveAllPdfTabBindings(all);
    FreeAllPdfTabBindings(all);
}

static bool BookWantsAiPanelOpen(i64 bookId) {
    if (bookId <= 0) {
        return true; // historical default: AI sidebar open
    }
    PdfTabBinding bind = LoadPdfTabBindingForBook(bookId);
    bool want = bind.aiOpen != 0; // -1 or 1 → open; 0 → closed
    FreePdfTabBinding(&bind);
    return want;
}

static void ApplyBookAiPanelVisibility(MainWindow* win, i64 bookId, LibraryBookKind kind) {
    if (!win || bookId <= 0) {
        return;
    }
    bool want = BookWantsAiPanelOpen(bookId);
    if (want) {
        if (win->CurrentTab()) {
            AIChatSetTabPanelOpen(win->CurrentTab(), AIChatBackend::None);
            AIChatSyncPanelsToCurrentTab(win);
        }
        win->uiState.aiChatVisible = false;
        win->uiState.webPanelVisible = true;
        EnsureWebPanelWebView(win);
        RestoreBookAiBindings(win, bookId, kind);
    } else if (win->uiState.webPanelVisible) {
        // Close without rewriting aiOpen (already stored for this book).
        win->uiState.webPanelVisible = false;
        for (int i = 0; i < len(win->webPanelTabs); i++) {
            WebviewWnd* wv = win->webPanelTabs[i].wv;
            if (!wv) {
                continue;
            }
            wv->SetControllerVisible(false, false);
        }
    }
    win->libraryAiStateReady = true;
    win->aiAppliedBookId = bookId;
}

// One-shot: older builds stored the center browser tab in webTab* for Web books.
static void MigrateLegacyWebBrowserBinding(PdfTabBinding* bind, i64 bookId) {
    if (!bind || bookId <= 0) {
        return;
    }
    if (bind->browserTabId || bind->browserTabUrl) {
        return;
    }
    if (!bind->webTabId && !bind->webTabUrl) {
        return;
    }
    TempStr key = fmt("%lld", bookId);
    Vec<PdfTabBinding> all = LoadAllPdfTabBindings();
    PdfTabBinding* b = FindPdfTabBinding(&all, key);
    if (!b) {
        PdfTabBinding nb{};
        nb.bookKey = str::Dup(key);
        all.Append(nb);
        b = &all.Last();
    }
    str::ReplaceWithCopy(&b->browserTabId, bind->webTabId);
    str::ReplaceWithCopy(&b->browserTabUrl, bind->webTabUrl);
    str::Free(b->webTabId);
    str::Free(b->webTabUrl);
    b->webTabId = {};
    b->webTabUrl = {};
    str::ReplaceWithCopy(&bind->browserTabId, b->browserTabId);
    str::ReplaceWithCopy(&bind->browserTabUrl, b->browserTabUrl);
    str::Free(bind->webTabId);
    str::Free(bind->webTabUrl);
    bind->webTabId = {};
    bind->webTabUrl = {};
    SaveAllPdfTabBindings(all);
    FreeAllPdfTabBindings(all);
}

int FindWebPanelTabById(MainWindow* win, Str id) {
    if (!win || !id) {
        return -1;
    }
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        if (str::Eq(win->webPanelTabs[i].id, id)) {
            return i;
        }
    }
    return -1;
}

// Activate by stable tab id; if the live tab was closed, reopen from saved URL keeping the same id.
bool ActivateOrRecreateWebPanelTab(MainWindow* win, Str tabId, Str tabUrl, Str titleFallback, bool rememberPdf) {
    if (!win) {
        return false;
    }
    if (tabId) {
        int idx = FindWebPanelTabById(win, tabId);
        if (idx >= 0) {
            ActivateWebPanelTabByIndex(win, idx, rememberPdf);
            return true;
        }
    }
    if (tabUrl && tabUrl.len > 0) {
        CreateNewWebPanelTab(win, tabUrl, titleFallback && titleFallback.len > 0 ? titleFallback : TitleFromUrlTemp(tabUrl),
                             tabId);
        return true;
    }
    return false;
}

void RememberPdfActiveTab(MainWindow* win) {
    if (!win || win->webPanelActiveTab < 0 || win->webPanelActiveTab >= len(win->webPanelTabs)) {
        return;
    }
    if (!LibraryIsAvailable()) {
        return;
    }
    i64 bookId = win->activeLibraryBookId;
    LibraryBook* book = nullptr;
    if (bookId <= 0) {
        WindowTab* tab = win->CurrentTab();
        if (!tab || !tab->filePath) {
            return;
        }
        book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
        if (!book) {
            return;
        }
        bookId = book->id;
    }
    WebPanelTab& t = win->webPanelTabs[win->webPanelActiveTab];
    // AI panel tabs are per library entry (PDF and Web alike).
    UpsertPdfTabBinding(bookId, t.id, t.url, {}, {}, true, false, false, false);
    if (IsNotebookLmUrl(t.url)) {
        UpsertPdfTabBinding(bookId, {}, {}, t.id, t.url, false, true, false, false);
    }
    DeleteLibraryBook(book);
    SaveWebPanelTabs(win);
}

void RestorePdfActiveTab(MainWindow* win) {
    if (!win || !LibraryIsAvailable()) {
        return;
    }
    i64 bookId = win->activeLibraryBookId;
    if (bookId <= 0) {
        WindowTab* tab = win->CurrentTab();
        if (!tab || !tab->filePath) {
            return;
        }
        LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
        if (!book) {
            return;
        }
        bookId = book->id;
        DeleteLibraryBook(book);
    }
    PdfTabBinding bind = LoadPdfTabBindingForBook(bookId);
    // Prefer stable tab id; fall back to saved URL if the tab was closed from the menu.
    ActivateOrRecreateWebPanelTab(win, bind.webTabId, bind.webTabUrl, TitleFromUrlTemp(bind.webTabUrl), false);
    FreePdfTabBinding(&bind);
}

void RestoreBookAiBindings(MainWindow* win, i64 bookId, LibraryBookKind kind) {
    if (!win || bookId <= 0 || !win->uiState.webPanelVisible) {
        return;
    }
    PdfTabBinding bind = LoadPdfTabBindingForBook(bookId);
    if (kind == LibraryBookKind::Web) {
        MigrateLegacyWebBrowserBinding(&bind, bookId);
    }
    // Prefer last AI "当前" tab; fall back to NotebookLM if that is all we have.
    if (bind.webTabId || bind.webTabUrl) {
        ActivateOrRecreateWebPanelTab(win, bind.webTabId, bind.webTabUrl, TitleFromUrlTemp(bind.webTabUrl), false);
    } else if (bind.notebookTabId || bind.notebookTabUrl) {
        ActivateOrRecreateWebPanelTab(win, bind.notebookTabId, bind.notebookTabUrl, StrL("NotebookLM"), false);
    } else {
        // First open for this book — land on NotebookLM so the AI pane is never blank.
        ActivateOrRecreateWebPanelTab(win, {}, StrL("https://notebook.google.com/"), StrL("NotebookLM"), false);
    }
    FreePdfTabBinding(&bind);
}

void RestorePdfNotebookLmTab(MainWindow* win) {
    if (!win || !LibraryIsAvailable()) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        // No PDF → open generic NotebookLM home.
        ActivateOrRecreateWebPanelTab(win, {}, StrL("https://notebook.google.com/"), StrL("NotebookLM"), false);
        return;
    }
    LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
    if (!book) {
        ActivateOrRecreateWebPanelTab(win, {}, StrL("https://notebook.google.com/"), StrL("NotebookLM"), false);
        return;
    }
    PdfTabBinding bind = LoadPdfTabBindingForBook(book->id);
    DeleteLibraryBook(book);
    bool ok = ActivateOrRecreateWebPanelTab(win, bind.notebookTabId, bind.notebookTabUrl,
                                            StrL("NotebookLM"), true);
    if (!ok) {
        ActivateOrRecreateWebPanelTab(win, {}, StrL("https://notebook.google.com/"), StrL("NotebookLM"), true);
    }
    FreePdfTabBinding(&bind);
}

TempStr EscapeJsonTemp(Str s) {
    str::Builder out;
    out.AppendChar('"');
    if (s) {
        for (int i = 0; i < s.len; i++) {
            char c = s.s[i];
            if (c == '"' || c == '\\') {
                out.AppendChar('\\');
                out.AppendChar(c);
            } else if (c == '\n') {
                out.Append(StrL("\\n"));
            } else if (c == '\r') {
                out.Append(StrL("\\r"));
            } else if ((u8)c < 0x20) {
                out.Append(fmt("\\u%04x", (unsigned)(u8)c));
            } else {
                out.AppendChar(c);
            }
        }
    }
    out.AppendChar('"');
    return ToStrTemp(out);
}

void WriteBridgeJson(MainWindow* win) {
    EnsureWebPanelDataLayout();
    TempStr pdfPath = {};
    TempStr fileName = {};
    if (win && win->CurrentTab() && win->CurrentTab()->filePath) {
        pdfPath = win->CurrentTab()->filePath;
        fileName = path::GetBaseNameTemp(pdfPath);
    }
    TempStr url = win && win->webPanelCurrentUrl ? win->webPanelCurrentUrl : gLastUrl;
    TempStr profile = WebViewProfileDirTemp();
    int port = win && win->webPanelCdpPort > 0 ? win->webPanelCdpPort : gCdpPort;
    TempStr json = fmt(
        "{\n"
        "  \"cdpPort\": %d,\n"
        "  \"cdpEndpoint\": \"http://127.0.0.1:%d\",\n"
        "  \"userDataDir\": %s,\n"
        "  \"url\": %s,\n"
        "  \"pdfPath\": %s,\n"
        "  \"fileName\": %s\n"
        "}\n",
        port, port, EscapeJsonTemp(profile), EscapeJsonTemp(url), EscapeJsonTemp(pdfPath), EscapeJsonTemp(fileName));
    file::WriteFile(BridgePathCanonicalTemp(), json);
}

void LayoutWebPanelBox(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox || !win->webPanelLayout) {
        return;
    }
    Rect rc = HwndClientRect(win->hwndWebPanelBox);
    LayoutTreeToSize(win->hwndWebPanelBox, win->webPanelLayout, {rc.dx, rc.dy}, &win->webPanelRoot);
    ShowActiveWebPanelTab(win);
}

void ShowActiveWebPanelTab(MainWindow* win) {
    if (!win || !win->webPanelWebViewSlot) {
        return;
    }
    if (!win->uiState.webPanelVisible) {
        for (int i = 0; i < len(win->webPanelTabs); i++) {
            WebviewWnd* wv = win->webPanelTabs[i].wv;
            if (!wv) {
                continue;
            }
            wv->SetControllerVisible(false);
            wv->SetIsVisible(false);
            if (wv->hwnd) {
                ShowWindow(wv->hwnd, SW_HIDE);
            }
        }
        return;
    }
    Rect wr = win->webPanelWebViewSlot->lastBounds;
    if (wr.dx < 1) {
        wr.dx = 1;
    }
    if (wr.dy < 1) {
        wr.dy = 1;
    }
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        WebviewWnd* wv = win->webPanelTabs[i].wv;
        if (!wv || !wv->hwnd) {
            continue;
        }
        bool active = (i == win->webPanelActiveTab);
        if (active) {
            // stay strictly in the slot below the header so pin clicks work
            MoveWindow(wv->hwnd, wr.x, wr.y, wr.dx, wr.dy, TRUE);
            wv->SetIsVisible(true);
            wv->SetControllerVisible(true);
            wv->UpdateWebviewSize();
            win->webPanelWebView = wv;
        } else {
            wv->SetControllerVisible(false);
            wv->SetIsVisible(false);
            ShowWindow(wv->hwnd, SW_HIDE);
        }
    }
}

int FindWebPanelTab(MainWindow* win, Str url) {
    if (!win || !url) {
        return -1;
    }
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        if (win->webPanelTabs[i].url && str::EqI(win->webPanelTabs[i].url, url)) {
            return i;
        }
    }
    return -1;
}

WebviewWnd* CreateWebPanelTabWebView(MainWindow* win, Str url) {
    if (!win || !url || !HasWebView()) {
        return nullptr;
    }
    EnsureWebPanelDataLayout();
    dir::CreateAll(WebViewProfileDirTemp());

    int port = gCdpPort > 0 ? gCdpPort : kDefaultCdpPort;
    win->webPanelCdpPort = port;

    auto* webView = new WebviewWnd();
    webView->events.ctx = win;
    webView->events.navigationStarting = OnWebNavStarting;
    webView->events.navigationCompleted = OnWebNavCompleted;
    webView->events.sourceChanged = OnWebSourceChanged;
    webView->events.documentTitleChanged = OnWebDocumentTitleChanged;
    webView->events.jsNotify = OnWebPanelJsNotify;
    webView->dataDir = str::Dup(WebViewProfileDirTemp());
    webView->useDedicatedEnvironment = true;
    webView->enableDevTools = true;
    webView->defaultBackgroundColor = kColWhite;
    webView->emulateMobile = true;
    webView->mobileDeviceWidth = 0;
    webView->mobileDeviceHeight = 0;
    webView->mobileDeviceScale = 1.0f;
    webView->userAgent = str::Dup(kMobileUserAgent);
    webView->dedicatedBrowserArgs = str::Dup(fmt("--remote-debugging-port=%d", port));
    webView->enableBrowserExtensions = true;
    webView->browserExtensionsDir = str::Dup(BrowserExtensionsInstalledDirTemp());
    webView->allowClipboardRead = true;
    webView->desiredVisible = false;
    webView->AddInitScript(kMobileViewportScript);

    CreateWebViewArgs wvArgs;
    wvArgs.parent = win->hwndWebPanelBox;
    wvArgs.pos = Rect(0, 0, 1, 1);
    webView->Create(wvArgs);
    if (!webView->hwnd) {
        delete webView;
        return nullptr;
    }
    ShowWindow(webView->hwnd, SW_HIDE);
    webView->Navigate(url);
    return webView;
}

void ActivateWebPanelTabByIndex(MainWindow* win, int idx, bool rememberPdf) {
    if (!win || idx < 0 || idx >= len(win->webPanelTabs)) {
        return;
    }
    WebPanelTab& t = win->webPanelTabs[idx];
    if (!t.wv && t.url) {
        t.wv = CreateWebPanelTabWebView(win, t.url);
        if (!t.wv) {
            return;
        }
    }
    win->webPanelActiveTab = idx;
    if (t.url) {
        str::ReplaceWithCopy(&win->webPanelCurrentUrl, t.url);
        str::ReplaceWithCopy(&gLastUrl, t.url);
    }
    win->webPanelWebView = t.wv;
    win->webPanelWebViewReady = t.wv != nullptr;
    ShowActiveWebPanelTab(win);
    SaveWebPanelTabs(win);
    WriteBridgeJson(win);
    RelayoutWebPanel(win);
    if (rememberPdf) {
        RememberPdfActiveTab(win);
    }
}

void CreateNewWebPanelTab(MainWindow* win, Str url, Str title, Str forcedId) {
    if (!win || !url) {
        return;
    }
    // If forcedId already exists as a live tab, just activate it (do not duplicate).
    if (forcedId) {
        int existing = FindWebPanelTabById(win, forcedId);
        if (existing >= 0) {
            ActivateWebPanelTabByIndex(win, existing, true);
            return;
        }
    }
    WebviewWnd* wv = CreateWebPanelTabWebView(win, url);
    if (!wv) {
        return;
    }
    WebPanelTab tab;
    tab.id = str::Dup(forcedId && forcedId.len > 0 ? forcedId : NewWebPanelTabIdTemp());
    tab.url = str::Dup(url);
    tab.title = str::Dup(title && title.len > 0 ? title : TitleFromUrlTemp(url));
    tab.wv = wv;
    win->webPanelTabs.Append(tab);
    ActivateWebPanelTabByIndex(win, len(win->webPanelTabs) - 1, true);
}

void CloseWebPanelTabAt(MainWindow* win, int idx) {
    if (!win || idx < 0 || idx >= len(win->webPanelTabs)) {
        return;
    }
    // Closing a live tab does NOT clear per-PDF bindings (id/url stay so restore can reopen).
    WebPanelTab& t = win->webPanelTabs[idx];
    delete t.wv;
    str::Free(t.id);
    str::Free(t.url);
    str::Free(t.title);
    win->webPanelTabs.RemoveAt(idx);
    if (win->webPanelActiveTab == idx) {
        win->webPanelActiveTab = -1;
        win->webPanelWebView = nullptr;
        win->webPanelWebViewReady = false;
        if (len(win->webPanelTabs) > 0) {
            int next = idx < len(win->webPanelTabs) ? idx : len(win->webPanelTabs) - 1;
            ActivateWebPanelTabByIndex(win, next, true);
        }
    } else if (win->webPanelActiveTab > idx) {
        win->webPanelActiveTab--;
    }
    SaveWebPanelTabs(win);
    ShowActiveWebPanelTab(win);
    RelayoutWebPanel(win);
}

void CloseAllWebPanelTabs(MainWindow* win) {
    if (!win) {
        return;
    }
    for (int i = len(win->webPanelTabs) - 1; i >= 0; i--) {
        WebPanelTab& t = win->webPanelTabs[i];
        delete t.wv;
        str::Free(t.id);
        str::Free(t.url);
        str::Free(t.title);
    }
    win->webPanelTabs.Clear();
    win->webPanelActiveTab = -1;
    win->webPanelWebView = nullptr;
    win->webPanelWebViewReady = false;
    SaveWebPanelTabs(win);
    ShowActiveWebPanelTab(win);
    RelayoutWebPanel(win);
}

void ActivateWebPanelTab(MainWindow* win, Str url) {
    if (!win || !url) {
        return;
    }
    int idx = FindWebPanelTab(win, url);
    // Reuse NotebookLM tab for automation (Add PDF) — avoid white-screen duplicates.
    if (idx < 0 && (str::ContainsI(url, StrL("notebook.google.com")) ||
                    str::ContainsI(url, StrL("notebooklm.google")))) {
        for (int i = 0; i < len(win->webPanelTabs); i++) {
            Str u = win->webPanelTabs[i].url;
            if (u && (str::ContainsI(u, StrL("notebook.google.com")) ||
                      str::ContainsI(u, StrL("notebooklm.google")) || str::EqI(u, StrL("about:blank")))) {
                idx = i;
                str::ReplaceWithCopy(&win->webPanelTabs[i].url, url);
                if (win->webPanelTabs[i].wv) {
                    win->webPanelTabs[i].wv->Navigate(url);
                }
                break;
            }
        }
    }
    if (idx >= 0) {
        ActivateWebPanelTabByIndex(win, idx, true);
        return;
    }
    CreateNewWebPanelTab(win, url, TitleFromUrlTemp(url));
}

void NavigateWebPanel(MainWindow* win, Str url) {
    ActivateWebPanelTab(win, url);
}

void OnWebPanelRefresh(MainWindow* win) {
    if (!win) {
        return;
    }
    // Point at the visible AI tab before reload (multi-tab can leave a stale ptr).
    ShowActiveWebPanelTab(win);
    if (!win->webPanelWebView) {
        return;
    }
    win->webPanelWebView->Reload(true);
}

void OnFocusCurrentPdfNotebookLm(MainWindow* win) {
    if (!win) {
        return;
    }
    // Center Web (middle pane) XOR PDF — same scope as "添加到 NotebookLM".
    if (win->uiState.webBrowserVisible) {
        TempStr url = win->webBrowserCurrentUrl;
        TempStr title = {};
        if (win->webBrowserActiveTab >= 0 && win->webBrowserActiveTab < len(win->webBrowserTabs)) {
            WebPanelTab& t = win->webBrowserTabs[win->webBrowserActiveTab];
            if (!url && t.url) {
                url = str::DupTemp(t.url);
            }
            if (t.title) {
                title = str::DupTemp(t.title);
            }
        }
        if (!url || str::EqI(url, StrL("about:blank"))) {
            MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("请先在中间栏打开一个网页。")),
                        CWStrTemp(_TRA("仅与当前来源对话")), MB_OK | MB_ICONINFORMATION);
            return;
        }
        i64 bookId = win->activeLibraryBookId > 0 ? win->activeLibraryBookId : 0;
        if (bookId <= 0 && LibraryIsAvailable()) {
            LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), url);
            if (book) {
                bookId = book->id;
                if ((!title || !title.len) && book->title) {
                    title = str::DupTemp(book->title);
                }
                DeleteLibraryBook(book);
            }
        }
        TempStr srcTitle = title ? title : TitleFromUrlTemp(url);
        WebPanelSelectNotebookLmSource(win, bookId, url, srcTitle);
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath) {
        MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("请先在中间栏打开一个 PDF 或网页。")),
                    CWStrTemp(_TRA("仅与当前来源对话")), MB_OK | MB_ICONINFORMATION);
        return;
    }
    i64 bookId = 0;
    LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
    if (book) {
        bookId = book->id;
        DeleteLibraryBook(book);
    }
    TempStr title = path::GetBaseNameTemp(tab->filePath);
    // Manual 1:1 focus — select only this PDF's source and open Chat. No auto-jump on PDF switch.
    WebPanelSelectNotebookLmSource(win, bookId, tab->filePath, title);
}

constexpr int kWebPanelMinDx = 150;
constexpr int kWebPanelMinDocDx = 200;

void OnWebPanelSplitterMove(VirtSplitter::MoveEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->w->GetHwnd());
    if (!win) {
        return;
    }
    Point pcur = HwndGetCursorPos(win->hwndFrame);
    Rect rFrame = HwndClientRect(win->hwndFrame);
    // Web panel is on the right: width = frameRight - cursorX
    int dx = rFrame.dx - pcur.x;
    // Allow nearly full-window web view; only keep a slim PDF canvas
    int maxDx = std::max(kWebPanelMinDx, rFrame.dx - kWebPanelMinDocDx);
    if (dx < kWebPanelMinDx || dx > maxDx) {
        ev->resizeAllowed = false;
        return;
    }
    if (ev->queryOnly) {
        return;
    }
    win->webPanelDx = dx;
    win->aiChatDx = dx;
    gGlobalPrefs->aiChatSidebarDx = dx;
    if (ev->finishedDragging) {
        SaveSettings();
        // force RelayoutFrame even if previous snapshot matched
        win->uiState.layout = {};
        ScheduleUiUpdate(win, kUiRelayout | kUiNoToolbars);
    }
}

int FindBookmarkByUrl(Str url) {
    if (!url) {
        return -1;
    }
    for (int i = 0; i < len(gBookmarks); i++) {
        if (str::Eq(gBookmarks[i].url, url)) {
            return i;
        }
    }
    return -1;
}

void AddOrUpdateBookmark(MainWindow* win, Str title, Str url, bool pinned) {
    if (!url || !str::StartsWithI(url, StrL("http"))) {
        return;
    }
    int idx = FindBookmarkByUrl(url);
    if (idx >= 0) {
        if (title && !str::Eq(gBookmarks[idx].title, title)) {
            str::ReplaceWithCopy(&gBookmarks[idx].title, title);
        }
        if (pinned) {
            gBookmarks[idx].pinned = true;
        }
    } else {
        WebBookmark b;
        b.title = str::Dup(title && title.len > 0 ? title : url);
        b.url = str::Dup(url);
        b.pinned = pinned;
        gBookmarks.Append(b);
    }
    SaveBookmarks();
    RebuildPinStrip(win);
    RelayoutWebPanel(win);
}

void DeleteBookmarkAt(MainWindow* win, int idx) {
    if (idx < 0 || idx >= len(gBookmarks)) {
        return;
    }
    Str url = gBookmarks[idx].url;
    if (win) {
        int tabIdx = FindWebPanelTab(win, url);
        if (tabIdx >= 0) {
            delete win->webPanelTabs[tabIdx].wv;
            str::Free(win->webPanelTabs[tabIdx].url);
            win->webPanelTabs.RemoveAt(tabIdx);
            if (win->webPanelActiveTab == tabIdx) {
                win->webPanelActiveTab = -1;
                win->webPanelWebView = nullptr;
                win->webPanelWebViewReady = false;
            } else if (win->webPanelActiveTab > tabIdx) {
                win->webPanelActiveTab--;
            }
        }
    }
    str::Free(gBookmarks[idx].title);
    str::Free(gBookmarks[idx].url);
    gBookmarks.RemoveAt(idx);
    EnsureDefaultBookmarks();
    SaveBookmarks();
    RebuildPinStrip(win);
    RelayoutWebPanel(win);
    if (win && win->webPanelActiveTab < 0 && len(gBookmarks) > 0) {
        ActivateWebPanelTab(win, gBookmarks[0].url);
    }
}

void TogglePinBookmarkAt(MainWindow* win, int idx) {
    if (idx < 0 || idx >= len(gBookmarks)) {
        return;
    }
    gBookmarks[idx].pinned = !gBookmarks[idx].pinned;
    SaveBookmarks();
    RebuildPinStrip(win);
    RelayoutWebPanel(win);
}

struct PinClickCtx {
    MainWindow* win = nullptr;
    int idx = 0;
};

Vec<PinClickCtx*> gPinClickCtxs;

void FreePinClickCtxs() {
    for (PinClickCtx* c : gPinClickCtxs) {
        delete c;
    }
    gPinClickCtxs.Reset();
}

// Horizontal pin strip: clips overflow and scrolls with the mouse wheel.
struct VirtHScrollStrip : VirtCtrl {
    int scrollX = 0;
    int contentDx = 0;
    int gap = 0;

    VirtHScrollStrip() {
        flags |= vwfClipChildren;
        onMouseWheel = MkMethod1<VirtHScrollStrip, VirtMouseEvent*, &VirtHScrollStrip::OnWheel>(this);
        gap = DpiScale(2);
    }

    Size GetIdealSize() override {
        int h = DpiScale(kPinIconPx + 4);
        int w = 0;
        for (VirtCtrl* c : children) {
            Size s = c->GetIdealSize();
            w += s.dx;
            if (h < s.dy) {
                h = s.dy;
            }
        }
        if (len(children) > 1) {
            w += gap * (len(children) - 1);
        }
        contentDx = w;
        return {w, h};
    }

    int MinIntrinsicWidth(int) override {
        // take whatever flex width the header HBox assigns; we scroll inside
        return 0;
    }

    int MinIntrinsicHeight(int) override {
        return GetIdealSize().dy;
    }

    Size Layout(Constraints bc) override {
        Size ideal = GetIdealSize();
        int dx = bc.max.dx < Inf ? bc.max.dx : ideal.dx;
        return bc.Constrain({dx, ideal.dy});
    }

    void ClampScroll() {
        int view = bounds.dx;
        int maxX = contentDx - view;
        if (maxX < 0) {
            maxX = 0;
        }
        if (scrollX < 0) {
            scrollX = 0;
        }
        if (scrollX > maxX) {
            scrollX = maxX;
        }
    }

    void LayoutChildren() {
        GetIdealSize(); // refresh contentDx
        ClampScroll();
        Rect r = lastBounds;
        int x = r.x - scrollX;
        for (VirtCtrl* c : children) {
            Size s = c->GetIdealSize();
            int y = r.y + (r.dy - s.dy) / 2;
            c->SetBounds({x, y, s.dx, s.dy});
            x += s.dx + gap;
        }
    }

    void SetBounds(Rect r) override {
        VirtCtrl::SetBounds(r);
        LayoutChildren();
    }

    void OnWheel(VirtMouseEvent* ev) {
        if (!ev) {
            return;
        }
        scrollX -= ev->wheelDelta / 4;
        LayoutChildren();
        Invalidate();
        ev->didHandle = true;
    }
};

// Favicon pin that scales the image to the header and shows a hover ✕ to unpin/delete.
struct VirtPinBtn : VirtCtrl {
    Pixmap* pixmap = nullptr; // owned if ownsPixmap
    bool ownsPixmap = false;
    MainWindow* win = nullptr;
    int bookmarkIdx = -1;
    bool hoverClose = false;
    bool selected = false;

    VirtPinBtn() {
        cursor = CursorId::Hand;
        onMouseEnter = MkMethod0<VirtPinBtn, &VirtPinBtn::OnEnter>(this);
        onMouseLeave = MkMethod0<VirtPinBtn, &VirtPinBtn::OnLeave>(this);
        onMouseMove = MkMethod1<VirtPinBtn, VirtMouseEvent*, &VirtPinBtn::OnMove>(this);
        onClick = MkMethod1<VirtPinBtn, VirtMouseEvent*, &VirtPinBtn::OnPinClick>(this);
    }

    ~VirtPinBtn() override {
        if (ownsPixmap) {
            FreePixmap(pixmap);
            pixmap = nullptr;
        }
    }

    Size GetIdealSize() override {
        int sz = DpiScale(kPinIconPx + 4);
        return {sz, sz};
    }

    Rect CloseRectLocal() const {
        int c = DpiScale(10);
        return {bounds.dx - c, 0, c, c};
    }

    void Paint(VirtPaintCtx& ctx) override {
        if (selected) {
            ctx.gfx->FillRect(ctx.bounds, MkGray(0xd0));
        } else if (HasFlag(vwfHovered)) {
            ctx.gfx->FillRect(ctx.bounds, MkGray(0xe8));
        }
        int icon = DpiScale(kPinIconPx);
        Rect dst = {ctx.bounds.x + (ctx.bounds.dx - icon) / 2, ctx.bounds.y + (ctx.bounds.dy - icon) / 2, icon, icon};
        if (pixmap) {
            ctx.gfx->DrawPixmap(pixmap, dst);
        }
        if (HasFlag(vwfHovered)) {
            Rect cr = CloseRectLocal();
            cr.Offset(ctx.bounds.x, ctx.bounds.y);
            Color circle = MkRgba(0xc0, 0x40, 0x40, 0xff);
            ctx.gfx->FillEllipse(cr, circle, 230);
            Color xcol = MkRgba(0xff, 0xff, 0xff, 0xff);
            int pad = cr.dx / 3;
            ctx.gfx->DrawLineAA({cr.x + pad, cr.y + pad}, {cr.x + cr.dx - pad, cr.y + cr.dy - pad}, xcol, 1.5f);
            ctx.gfx->DrawLineAA({cr.x + cr.dx - pad, cr.y + pad}, {cr.x + pad, cr.y + cr.dy - pad}, xcol, 1.5f);
        }
    }

    void OnEnter() {
        Invalidate();
    }
    void OnLeave() {
        hoverClose = false;
        Invalidate();
    }
    void OnMove(VirtMouseEvent* ev) {
        if (!ev) {
            return;
        }
        bool onClose = CloseRectLocal().Contains(ev->pt);
        if (onClose != hoverClose) {
            hoverClose = onClose;
            Invalidate();
        }
    }
    void OnPinClick(VirtMouseEvent* ev) {
        if (!ev || !win) {
            return;
        }
        if (CloseRectLocal().Contains(ev->pt)) {
            DeleteBookmarkAt(win, bookmarkIdx);
            ev->didHandle = true;
            return;
        }
        if (bookmarkIdx >= 0 && bookmarkIdx < len(gBookmarks)) {
            ActivateWebPanelTab(win, gBookmarks[bookmarkIdx].url);
        }
        ev->didHandle = true;
    }
};

void OnPinnedBookmarkClick(PinClickCtx* ctx) {
    if (!ctx || !ctx->win) {
        return;
    }
    if (ctx->idx < 0 || ctx->idx >= len(gBookmarks)) {
        return;
    }
    ActivateWebPanelTab(ctx->win, gBookmarks[ctx->idx].url);
}

void RebuildPinStrip(MainWindow* win) {
    // Pin favicon strip removed — bookmarks open from the menu only.
    (void)win;
}

void ShowBookmarksMenu(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    constexpr UINT kOpenBase = 1000;
    constexpr UINT kAddCurrent = 10;
    constexpr UINT kAddClipboard = 11;

    for (int i = 0; i < len(gBookmarks); i++) {
        TempWStr name = ToWStrTemp(gBookmarks[i].title ? gBookmarks[i].title : gBookmarks[i].url);
        AppendMenuW(menu, MF_STRING, kOpenBase + i, name.s);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kAddCurrent, CWStrTemp(_TRA("添加当前页为书签")));
    AppendMenuW(menu, MF_STRING, kAddClipboard, CWStrTemp(_TRA("从剪贴板添加 URL")));

    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, win->hwndWebPanelBox, nullptr);
    DestroyMenu(menu);
    if (cmd == 0) {
        return;
    }
    if (cmd == (int)kAddCurrent) {
        if (win->webPanelCurrentUrl) {
            AddOrUpdateBookmark(win, win->webPanelCurrentUrl, win->webPanelCurrentUrl, false);
        }
        return;
    }
    if (cmd == (int)kAddClipboard) {
        TempStr clip = ClipboardTextTemp();
        while (clip.len > 0 && (clip.s[0] == ' ' || clip.s[0] == '\t' || clip.s[0] == '\r' || clip.s[0] == '\n')) {
            clip.s++;
            clip.len--;
        }
        while (clip.len > 0 &&
               (clip.s[clip.len - 1] == ' ' || clip.s[clip.len - 1] == '\t' || clip.s[clip.len - 1] == '\r' ||
                clip.s[clip.len - 1] == '\n')) {
            clip.len--;
        }
        if (clip.len > 0 && str::StartsWithI(clip, StrL("http"))) {
            AddOrUpdateBookmark(win, clip, clip, false);
            CreateNewWebPanelTab(win, clip, TitleFromUrlTemp(clip));
        }
        return;
    }
    if (cmd >= (int)kOpenBase && cmd < (int)kOpenBase + len(gBookmarks)) {
        // Bookmark click always opens a NEW tab (does not reuse / navigate existing).
        WebBookmark& b = gBookmarks[cmd - kOpenBase];
        CreateNewWebPanelTab(win, b.url, b.title);
    }
}

void OnBookmarksButton(MainWindow* win) {
    ShowBookmarksMenu(win);
}

// Custom tabs popup: left-click activates, middle-click closes (no ✕ — matches browser middle-click close).
struct TabsPopupState {
    MainWindow* win = nullptr;
    HWND hwnd = nullptr;
    int hover = -100; // -2=close-all, >=0=tab index
    int rowH = 0;
    int padX = 0;
    int nTabs = 0;
    int statsH = 0;
    int closeAllY0 = 0;
    int tabsY0 = 0;
    bool hasCloseAll = false;
    // closing a tab can hand focus to a WebView a moment later; don't let that dismiss the
    // reopened popup (middle-click closes several tabs in a row)
    ULONGLONG keepOpenUntilMs = 0;
    Str stats;
    Str emptyHint;
    Str closeAllLabel;
    Vec<Str> titles;
    Vec<bool> activeFlags;
};

static TabsPopupState* gTabsPopup = nullptr;
static ATOM gTabsPopupAtom = 0;

static void DestroyTabsPopup() {
    if (!gTabsPopup) {
        return;
    }
    TabsPopupState* st = gTabsPopup;
    gTabsPopup = nullptr;
    if (st->hwnd) {
        DestroyWindow(st->hwnd);
        st->hwnd = nullptr;
    }
    str::Free(st->stats);
    str::Free(st->emptyHint);
    str::Free(st->closeAllLabel);
    for (Str& t : st->titles) {
        str::Free(t);
    }
    delete st;
}

static int TabsPopupHitRow(TabsPopupState* st, int y) {
    if (!st) {
        return -100;
    }
    if (st->hasCloseAll && y >= st->closeAllY0 && y < st->closeAllY0 + st->rowH) {
        return -2;
    }
    if (y < st->tabsY0) {
        return -100;
    }
    int idx = (y - st->tabsY0) / st->rowH;
    if (idx < 0 || idx >= st->nTabs) {
        return -100;
    }
    return idx;
}

static void ShowTabsMenuAt(MainWindow* win, POINT pos, ULONGLONG keepOpenMs);
constexpr UINT kTabsPopupReactivateMsg = WM_APP + 0x51;

static LRESULT CALLBACK TabsPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TabsPopupState* st = gTabsPopup;
    if (!st || st->hwnd != hwnd) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                if (GetTickCount64() < st->keepOpenUntilMs) {
                    PostMessageW(hwnd, kTabsPopupReactivateMsg, 0, 0);
                    return 0;
                }
                DestroyTabsPopup();
            }
            return 0;
        case kTabsPopupReactivateMsg:
            SetForegroundWindow(hwnd);
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hit = TabsPopupHitRow(st, pt.y);
            if (hit != st->hover) {
                st->hover = hit;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hit = TabsPopupHitRow(st, pt.y);
            MainWindow* win = st->win;
            if (hit == -2) {
                DestroyTabsPopup();
                CloseAllWebPanelTabs(win);
                return 0;
            }
            if (hit >= 0) {
                DestroyTabsPopup();
                ActivateWebPanelTabByIndex(win, hit, true);
                return 0;
            }
            return 0;
        }
        case WM_MBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int hit = TabsPopupHitRow(st, pt.y);
            if (hit >= 0) {
                // stay open at the same spot: the next tab slides under the cursor
                MainWindow* win = st->win;
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                DestroyTabsPopup();
                CloseWebPanelTabAt(win, hit);
                if (IsMainWindowValid(win)) {
                    ShowTabsMenuAt(win, POINT{wr.left, wr.top}, 600);
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyTabsPopup();
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            Color bg = ThemeControlBackgroundColor();
            Color fg = ThemeWindowTextColor();
            Color muted = ThemeWindowTextDisabledColor();
            Color hot = ThemeHotBackgroundColor();
            Color edge = ThemeEdgeColor();
            HBRUSH brBg = CreateSolidBrush(bg);
            FillRect(hdc, &rc, brBg);
            DeleteObject(brBg);

            SetBkMode(hdc, TRANSPARENT);
            // Use normal menu font (sidebar label font is bold by design).
            PlatformFont* pfont = GetAppMenuFont();
            HFONT font = pfont ? pfont->GetHFont() : nullptr;
            HFONT oldFont = font ? (HFONT)SelectObject(hdc, font) : nullptr;

            auto drawTextRow = [&](int y, Str text, Color col, bool highlight, bool activeDot) {
                RECT row{0, y, rc.right, y + st->rowH};
                if (highlight) {
                    HBRUSH brHot = CreateSolidBrush(hot);
                    FillRect(hdc, &row, brHot);
                    DeleteObject(brHot);
                }
                SetTextColor(hdc, col);
                int left = st->padX;
                if (activeDot) {
                    const WCHAR* dot = L"\u25CF ";
                    TextOutW(hdc, left, y + (st->rowH - 16) / 2, dot, 2);
                    SIZE sz{};
                    GetTextExtentPoint32W(hdc, dot, 2, &sz);
                    left += sz.cx;
                }
                RECT textRc{left, y, rc.right - st->padX, y + st->rowH};
                TempWStr wText = ToWStrTemp(text);
                DrawTextW(hdc, wText.s, wText.len, &textRc,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            };

            drawTextRow(0, st->stats, muted, false, false);
            HPEN pen = CreatePen(PS_SOLID, 1, edge);
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            int sepY = st->statsH;
            MoveToEx(hdc, st->padX, sepY, nullptr);
            LineTo(hdc, rc.right - st->padX, sepY);

            if (st->hasCloseAll) {
                drawTextRow(st->closeAllY0, st->closeAllLabel, fg, st->hover == -2, false);
                int sep2 = st->closeAllY0 + st->rowH;
                MoveToEx(hdc, st->padX, sep2, nullptr);
                LineTo(hdc, rc.right - st->padX, sep2);
            }

            if (st->nTabs == 0) {
                drawTextRow(st->tabsY0, st->emptyHint, muted, false, false);
            } else {
                for (int i = 0; i < st->nTabs; i++) {
                    drawTextRow(st->tabsY0 + i * st->rowH, st->titles[i], fg, st->hover == i, st->activeFlags[i]);
                }
            }

            SelectObject(hdc, oldPen);
            DeleteObject(pen);
            if (oldFont) {
                SelectObject(hdc, oldFont);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            if (gTabsPopup && gTabsPopup->hwnd == hwnd) {
                gTabsPopup->hwnd = nullptr;
                DestroyTabsPopup();
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowTabsMenu(MainWindow* win) {
    POINT pt;
    GetCursorPos(&pt);
    ShowTabsMenuAt(win, pt, 0);
}

static void ShowTabsMenuAt(MainWindow* win, POINT pos, ULONGLONG keepOpenMs) {
    if (!win || !win->hwndWebPanelBox) {
        return;
    }
    DestroyTabsPopup();
    EnsureWebPanelDataLayout();
    LoadWebPanelTabs(win);
    // Refresh live URL/title from WebViews before listing (SPA may have pushState'd).
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        WebPanelTab& t = win->webPanelTabs[i];
        if (!t.wv) {
            continue;
        }
        TempStr url = t.wv->GetSourceTemp();
        TempStr title = t.wv->GetDocumentTitleTemp();
        SyncWebPanelTabFromWebView(win, t.wv, url, title);
    }
    UpdateWebPanelCpuSample();
    UpdateWebPanelCpuSample();

    auto* st = new TabsPopupState();
    st->win = win;
    st->nTabs = len(win->webPanelTabs);
    st->stats = str::Dup(WebPanelResourceStatsTemp());
    st->emptyHint = str::Dup(_TRA("（暂无 Tab — 点书签新建）"));
    st->closeAllLabel = str::Dup(_TRA("关闭所有标签页"));
    st->hasCloseAll = st->nTabs > 0;
    st->rowH = DpiScale(26);
    st->padX = DpiScale(10);
    st->statsH = st->rowH;
    st->closeAllY0 = st->statsH + 1;
    st->tabsY0 = st->hasCloseAll ? (st->closeAllY0 + st->rowH + 1) : (st->statsH + 1);
    for (int i = 0; i < st->nTabs; i++) {
        WebPanelTab& t = win->webPanelTabs[i];
        // Prefer live document.title; fall back to stored title / host.
        TempStr liveTitle = t.wv ? t.wv->GetDocumentTitleTemp() : TempStr{};
        Str shown = (liveTitle && liveTitle.len > 0 && !str::StartsWithI(liveTitle, StrL("http")))
                        ? Str(liveTitle)
                        : (t.title ? t.title : TitleFromUrlTemp(t.url));
        st->titles.Append(str::Dup(shown));
        st->activeFlags.Append(i == win->webPanelActiveTab);
    }

    if (!gTabsPopupAtom) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = TabsPopupWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SUMATRA_WEBPANEL_TABS_MENU";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.style = CS_DROPSHADOW;
        gTabsPopupAtom = RegisterClassW(&wc);
    }

    int height = st->statsH + 1 + (st->hasCloseAll ? st->rowH + 1 : 0) +
                 (st->nTabs > 0 ? st->nTabs * st->rowH : st->rowH) + DpiScale(4);
    int width = DpiScale(360);
    HDC hdcScreen = GetDC(nullptr);
    if (hdcScreen) {
        PlatformFont* pfont = GetAppMenuFont();
        HFONT font = pfont ? pfont->GetHFont() : nullptr;
        HFONT old = font ? (HFONT)SelectObject(hdcScreen, font) : nullptr;
        SIZE sz{};
        TempWStr wStats = ToWStrTemp(st->stats);
        GetTextExtentPoint32W(hdcScreen, wStats.s, wStats.len, &sz);
        width = std::max(width, (int)sz.cx + st->padX * 2);
        if (st->hasCloseAll) {
            TempWStr wClose = ToWStrTemp(st->closeAllLabel);
            GetTextExtentPoint32W(hdcScreen, wClose.s, wClose.len, &sz);
            width = std::max(width, (int)sz.cx + st->padX * 2);
        }
        for (Str& title : st->titles) {
            TempWStr wTitle = ToWStrTemp(title);
            GetTextExtentPoint32W(hdcScreen, wTitle.s, wTitle.len, &sz);
            width = std::max(width, (int)sz.cx + st->padX * 2 + DpiScale(24));
        }
        if (old) {
            SelectObject(hdcScreen, old);
        }
        ReleaseDC(nullptr, hdcScreen);
    }
    width = std::min(width, DpiScale(560));

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SUMATRA_WEBPANEL_TABS_MENU", L"",
                                WS_POPUP | WS_BORDER, pos.x, pos.y, width, height, win->hwndFrame, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        for (Str& t : st->titles) {
            str::Free(t);
        }
        str::Free(st->stats);
        str::Free(st->emptyHint);
        str::Free(st->closeAllLabel);
        delete st;
        return;
    }
    st->hwnd = hwnd;
    st->keepOpenUntilMs = keepOpenMs ? GetTickCount64() + keepOpenMs : 0;
    POINT cur;
    GetCursorPos(&cur);
    st->hover = TabsPopupHitRow(st, cur.y - pos.y);
    gTabsPopup = st;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

void OnTabsButton(MainWindow* win) {
    ShowTabsMenu(win);
}

// WebView2 / Edge Chromium history (equivalent to Chrome chrome://history/).
void OnWebPanelHistoryButton(MainWindow* win) {
    if (!win) {
        return;
    }
    win->uiState.aiChatVisible = false;
    win->uiState.webPanelVisible = true;
    EnsureWebPanelWebView(win);
    CreateNewWebPanelTab(win, StrL("edge://history/"), _TRA("历史记录"));
    ScheduleUiUpdate(win);
}

void OnNotebookLmTabButton(MainWindow* win);
void OnFocusCurrentPdfNotebookLm(MainWindow* win);

struct BrowserPluginEntry {
    Str id;
    Str name;
    Str kind;         // "host" | "webview2" | "webview2-overlay"
    Str browser;      // "Browser-AIChat" | "Browser-Library" | empty (= all)
    Str action;       // host action id
    Str folder;
    Str chromeId;     // store id for chrome-extension:// options
    Str optionsPage;  // e.g. index.html
};

static void FreeBrowserPluginEntry(BrowserPluginEntry* e) {
    if (!e) {
        return;
    }
    str::Free(e->id);
    str::Free(e->name);
    str::Free(e->kind);
    str::Free(e->browser);
    str::Free(e->action);
    str::Free(e->folder);
    str::Free(e->chromeId);
    str::Free(e->optionsPage);
    *e = {};
}

static TempStr ReadPluginJsonFieldTemp(Str json, Str field) {
    if (!json || !field) {
        return {};
    }
    struct St {
        Str field;
        Str out;
    } st{field};
    auto onVal = [](St* s, json::Value* v) {
        if (v->type != json::Type::String || !v->value) {
            return;
        }
        TempStr seg = str::JoinTemp(StrL("/"), s->field);
        if (json::PathMatch(v->path, seg)) {
            str::ReplaceWithCopy(&s->out, v->value);
            v->stop = true;
        }
    };
    json::Parse(json, MkFunc1<St, json::Value*>(onVal, &st));
    TempStr res = st.out ? str::DupTemp(st.out) : TempStr{};
    str::Free(st.out);
    return res;
}

static TempStr FindRepoExtensionsOverlaysTemp() {
    TempStr dir = GetSelfExeDirTemp();
    for (int i = 0; i < 8 && dir; i++) {
        TempStr candidate = path::JoinTemp(dir, StrL("extensions\\overlays"));
        if (dir::Exists(candidate)) {
            return candidate;
        }
        TempStr parent = path::GetDirTemp(dir);
        if (!parent || str::EqI(parent, dir)) {
            break;
        }
        dir = parent;
    }
    return {};
}

static bool ShCopyDirContents(Str dstDir, Str srcDir) {
    if (!dir::Exists(srcDir)) {
        return false;
    }
    dir::CreateAll(dstDir);
    TempStr fromPath = path::JoinTemp(srcDir, StrL("*"));
    TempWStr fromW = ToWStrTemp(fromPath);
    TempWStr toW = ToWStrTemp(dstDir);
    int nFrom = len(fromW) + 2;
    int nTo = len(toW) + 2;
    TempWStr fromZ = WStr(AllocArrayTemp<WCHAR>(nFrom), nFrom);
    TempWStr toZ = WStr(AllocArrayTemp<WCHAR>(nTo), nTo);
    wstr::BufSet(fromZ, fromW);
    wstr::BufSet(toZ, toW);
    SHFILEOPSTRUCTW shfo = {nullptr, FO_COPY, fromZ.s, toZ.s, FOF_NO_UI | FOF_NOCONFIRMMKDIR, FALSE, nullptr,
                            nullptr};
    int res = SHFileOperationW(&shfo);
    return res == 0 && !shfo.fAnyOperationsAborted;
}

static TempStr FindChromeUnpackedExtensionTemp(Str chromeId) {
    if (!chromeId) {
        return {};
    }
    TempStr local = GetEnvVariableTemp(StrL("LOCALAPPDATA"));
    if (!local) {
        return {};
    }
    TempStr best = {};
    TempStr bestVer = {};
    TempStr roots[] = {
        path::JoinTemp(local, StrL("Google\\Chrome\\User Data")),
        path::JoinTemp(local, StrL("Microsoft\\Edge\\User Data")),
    };
    for (TempStr root : roots) {
        if (!dir::Exists(root)) {
            continue;
        }
        DirIter profiles{root};
        profiles.includeDirs = true;
        profiles.includeFiles = false;
        for (DirIterEntry* pe : profiles) {
            if (!pe || !pe->isDir || !pe->name) {
                continue;
            }
            if (str::EqI(pe->name, StrL("System Profile")) || str::StartsWithI(pe->name, StrL("Guest"))) {
                continue;
            }
            TempStr extRoot = path::JoinTemp(pe->filePath, fmt("Extensions\\%s", chromeId));
            if (!dir::Exists(extRoot)) {
                continue;
            }
            DirIter vers{extRoot};
            vers.includeDirs = true;
            vers.includeFiles = false;
            for (DirIterEntry* ve : vers) {
                if (!ve || !ve->isDir || !ve->name) {
                    continue;
                }
                if (!file::Exists(path::JoinTemp(ve->filePath, StrL("manifest.json")))) {
                    continue;
                }
                if (!bestVer || str::CmpNatural(ve->name, bestVer) > 0) {
                    bestVer = str::DupTemp(ve->name);
                    best = str::DupTemp(ve->filePath);
                }
            }
        }
    }
    return best;
}

static void MaterializeChromeOverlay(Str overlayId, Str chromeId, Str name, Str browser, Str optionsPage,
                                     Str description) {
    if (!overlayId || !chromeId) {
        return;
    }
    TempStr src = FindChromeUnpackedExtensionTemp(chromeId);
    if (!src) {
        logf("MaterializeChromeOverlay: Chrome extension '%s' not found (install in Chrome first)\n", chromeId);
        return;
    }
    TempStr dst = path::JoinTemp(BrowserExtensionsOverlayDirTemp(), overlayId);
    TempStr stampPath = path::JoinTemp(dst, StrL(".sumatra-overlay-src"));
    TempStr stampWant = fmt("%s\n%s\n", chromeId, src);
    if (dir::Exists(dst) && file::Exists(path::JoinTemp(dst, StrL("manifest.json"))) && file::Exists(stampPath)) {
        Str prev = file::ReadFile(stampPath);
        bool same = prev && str::Eq(prev, stampWant);
        str::Free(prev);
        if (same) {
            return;
        }
    }
    if (dir::Exists(dst)) {
        dir::RemoveAll(dst);
    }
    dir::CreateAll(dst);
    if (!ShCopyDirContents(dst, src)) {
        logf("MaterializeChromeOverlay: copy failed '%s' → '%s'\n", src, dst);
        return;
    }
    TempStr meta = path::JoinTemp(dst, StrL("_metadata"));
    if (dir::Exists(meta)) {
        dir::RemoveAll(meta);
    }
    TempStr side = path::JoinTemp(dst, StrL("plugin.json"));
    TempStr body =
        fmt("{\n"
            "  \"id\": %s,\n"
            "  \"chromeId\": %s,\n"
            "  \"name\": %s,\n"
            "  \"kind\": \"webview2-overlay\",\n"
            "  \"browser\": %s,\n"
            "  \"optionsPage\": %s,\n"
            "  \"description\": %s\n"
            "}\n",
            EscapeJsonTemp(overlayId), EscapeJsonTemp(chromeId), EscapeJsonTemp(name ? name : overlayId),
            EscapeJsonTemp(browser ? browser : StrL("Browser-Library")),
            EscapeJsonTemp(optionsPage ? optionsPage : StrL("index.html")),
            EscapeJsonTemp(description ? description : StrL("")));
    file::WriteFile(side, body);
    file::WriteFile(stampPath, stampWant);
    logf("MaterializeChromeOverlay: '%s' ready at '%s'\n", overlayId, dst);
}

static void SyncAndMaterializeOverlays() {
    TempStr appOverlays = path::JoinTemp(BrowserExtensionsRootDirTemp(), StrL("overlays"));
    dir::CreateAll(appOverlays);
    dir::CreateAll(BrowserExtensionsOverlayDirTemp());

    TempStr repoOverlays = FindRepoExtensionsOverlaysTemp();
    if (repoOverlays) {
        DirIter di{repoOverlays};
        di.includeFiles = true;
        di.includeDirs = false;
        for (DirIterEntry* de : di) {
            if (!de || !de->name || !str::EndsWithI(de->name, StrL(".json"))) {
                continue;
            }
            file::Copy(path::JoinTemp(appOverlays, de->name), de->filePath, false);
        }
    }

    DirIter di{appOverlays};
    di.includeFiles = true;
    di.includeDirs = false;
    for (DirIterEntry* de : di) {
        if (!de || !de->name || !str::EndsWithI(de->name, StrL(".json"))) {
            continue;
        }
        Str raw = file::ReadFile(de->filePath);
        TempStr id = ReadPluginJsonFieldTemp(raw, StrL("id"));
        TempStr chromeId = ReadPluginJsonFieldTemp(raw, StrL("chromeId"));
        TempStr name = ReadPluginJsonFieldTemp(raw, StrL("name"));
        TempStr browser = ReadPluginJsonFieldTemp(raw, StrL("browser"));
        TempStr optionsPage = ReadPluginJsonFieldTemp(raw, StrL("optionsPage"));
        TempStr description = ReadPluginJsonFieldTemp(raw, StrL("description"));
        str::Free(raw);
        if (!id) {
            id = path::GetPathNoExtTemp(de->name);
        }
        if (!chromeId) {
            continue;
        }
        MaterializeChromeOverlay(id, chromeId, name, browser, optionsPage, description);
    }
}

static void CollectInstalledPluginsFromDir(Vec<BrowserPluginEntry>* out, Str browserFilter, Str root) {
    if (!out || !root || !dir::Exists(root)) {
        return;
    }
    DirIter di{root};
    di.includeDirs = true;
    di.includeFiles = false;
    for (DirIterEntry* de : di) {
        if (!de || !de->isDir || !de->name) {
            continue;
        }
        TempStr pluginJsonPath = path::JoinTemp(de->filePath, StrL("plugin.json"));
        TempStr manifestPath = path::JoinTemp(de->filePath, StrL("manifest.json"));
        BrowserPluginEntry e{};
        e.folder = str::Dup(de->filePath);
        e.id = str::Dup(de->name);
        if (file::Exists(pluginJsonPath)) {
            Str raw = file::ReadFile(pluginJsonPath);
            TempStr name = ReadPluginJsonFieldTemp(raw, StrL("name"));
            TempStr kind = ReadPluginJsonFieldTemp(raw, StrL("kind"));
            TempStr action = ReadPluginJsonFieldTemp(raw, StrL("action"));
            TempStr id = ReadPluginJsonFieldTemp(raw, StrL("id"));
            TempStr browser = ReadPluginJsonFieldTemp(raw, StrL("browser"));
            TempStr chromeId = ReadPluginJsonFieldTemp(raw, StrL("chromeId"));
            TempStr optionsPage = ReadPluginJsonFieldTemp(raw, StrL("optionsPage"));
            str::Free(raw);
            if (id) {
                str::ReplaceWithCopy(&e.id, id);
            }
            e.name = str::Dup(name ? name : de->name);
            e.kind = str::Dup(kind ? kind : StrL("host"));
            e.browser = str::Dup(browser);
            e.action = str::Dup(action);
            e.chromeId = str::Dup(chromeId);
            e.optionsPage = str::Dup(optionsPage);
            bool isOverlay = e.kind && (str::Eq(e.kind, StrL("webview2-overlay")) ||
                                        str::Eq(e.kind, StrL("webview2")));
            // Host plugins default to AI; overlays default to Library.
            TempStr want = e.browser ? e.browser : (isOverlay ? StrL("Browser-Library") : StrL("Browser-AIChat"));
            if (browserFilter && !str::EqI(want, browserFilter)) {
                FreeBrowserPluginEntry(&e);
                continue;
            }
            // Overlay without materialized manifest is not usable yet.
            if (isOverlay && !file::Exists(manifestPath)) {
                FreeBrowserPluginEntry(&e);
                continue;
            }
            out->Append(e);
            continue;
        }
        if (file::Exists(manifestPath)) {
            e.name = str::Dup(de->name);
            e.kind = str::Dup(StrL("webview2"));
            if (browserFilter && !str::EqI(browserFilter, StrL("Browser-Library")) &&
                !str::EqI(browserFilter, StrL("Browser-AIChat"))) {
                // keep
            }
            // Plain manifest folders: show on Library by default.
            if (browserFilter && !str::EqI(browserFilter, StrL("Browser-Library"))) {
                FreeBrowserPluginEntry(&e);
                continue;
            }
            out->Append(e);
            continue;
        }
        FreeBrowserPluginEntry(&e);
    }
}

static void CollectInstalledPlugins(Vec<BrowserPluginEntry>* out, Str browserFilter) {
    if (!out) {
        return;
    }
    EnsureBrowserExtensionsLayout();
    CollectInstalledPluginsFromDir(out, browserFilter, BrowserExtensionsInstalledDirTemp());
    CollectInstalledPluginsFromDir(out, browserFilter, BrowserExtensionsOverlayDirTemp());
}

// Resolve middle-pane target for NotebookLM host plugins: center Web XOR PDF.
static bool ResolveCenterNotebookLmTarget(MainWindow* win, i64* bookIdOut, TempStr* pdfPathOut, TempStr* sourceUrlOut,
                                          TempStr* titleOut) {
    if (!win || !bookIdOut || !pdfPathOut || !sourceUrlOut || !titleOut) {
        return false;
    }
    *bookIdOut = 0;
    *pdfPathOut = {};
    *sourceUrlOut = {};
    *titleOut = {};

    if (win->uiState.webBrowserVisible) {
        TempStr url = win->webBrowserCurrentUrl;
        TempStr title = {};
        if (win->webBrowserActiveTab >= 0 && win->webBrowserActiveTab < len(win->webBrowserTabs)) {
            WebPanelTab& t = win->webBrowserTabs[win->webBrowserActiveTab];
            if (!url && t.url) {
                url = str::DupTemp(t.url);
            }
            if (t.title) {
                title = str::DupTemp(t.title);
            }
        }
        if (!url || str::EqI(url, StrL("about:blank")) || str::StartsWithI(url, StrL("edge://"))) {
            return false;
        }
        *sourceUrlOut = url;
        *titleOut = title ? title : TitleFromUrlTemp(url);
        if (win->activeLibraryBookId > 0) {
            *bookIdOut = win->activeLibraryBookId;
        } else if (LibraryIsAvailable()) {
            LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), url);
            if (!book) {
                // Web books may store url separately; path can still be the URL key.
                book = LibraryStoreFindBookByPath(LibraryGetStore(), url);
            }
            if (book) {
                *bookIdOut = book->id;
                if ((!title || !title.len) && book->title) {
                    *titleOut = str::DupTemp(book->title);
                }
                DeleteLibraryBook(book);
            }
        }
        return true;
    }

    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath || !str::EndsWithI(tab->filePath, StrL(".pdf"))) {
        return false;
    }
    *pdfPathOut = str::DupTemp(tab->filePath);
    *titleOut = path::GetBaseNameTemp(tab->filePath);
    if (LibraryIsAvailable()) {
        LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
        if (book) {
            *bookIdOut = book->id;
            DeleteLibraryBook(book);
        }
    }
    return true;
}

static void RunHostPluginAction(MainWindow* win, Str action) {
    if (!win || !action) {
        return;
    }
    if (str::Eq(action, StrL("notebooklm.focus"))) {
        OnFocusCurrentPdfNotebookLm(win);
        return;
    }
    if (str::Eq(action, StrL("notebooklm.open-tab"))) {
        OnNotebookLmTabButton(win);
        return;
    }
    if (str::Eq(action, StrL("notebooklm.add"))) {
        i64 bookId = 0;
        TempStr pdfPath = {};
        TempStr sourceUrl = {};
        TempStr title = {};
        if (!ResolveCenterNotebookLmTarget(win, &bookId, &pdfPath, &sourceUrl, &title)) {
            MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("请先在中间栏打开一个 PDF 或网页。")),
                        CWStrTemp(_TRA("添加到 NotebookLM")), MB_OK | MB_ICONINFORMATION);
            return;
        }
        WebPanelAddToNotebookLm(win, bookId, pdfPath, sourceUrl, title);
        return;
    }
    logf("RunHostPluginAction: unknown action '%s'\n", action);
}

void ShowBrowserExtensionsMenu(MainWindow* win, Str browserProfile) {
    if (!win) {
        return;
    }
    EnsureBrowserExtensionsLayout();
    Vec<BrowserPluginEntry> plugins;
    CollectInstalledPlugins(&plugins, browserProfile);

    HMENU menu = CreatePopupMenu();
    constexpr UINT kManageId = 9000;
    for (int i = 0; i < len(plugins); i++) {
        TempStr label = plugins[i].name ? plugins[i].name : plugins[i].id;
        if (plugins[i].kind && (str::Eq(plugins[i].kind, StrL("webview2")) ||
                                str::Eq(plugins[i].kind, StrL("webview2-overlay")))) {
            label = fmt("🧩 %s", label);
        }
        AppendMenuW(menu, MF_STRING, (UINT)(i + 1), CWStrTemp(label));
    }
    if (len(plugins) == 0) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, CWStrTemp(_TRA("(尚未安装扩展)")));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kManageId, CWStrTemp(_TRA("管理扩展程序")));

    POINT pt{};
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    DestroyMenu(menu);

    if (cmd == (int)kManageId) {
        TempStr root = BrowserExtensionsRootDirTemp();
        SumatraOpenPathInDefaultFileManager(root);
    } else if (cmd >= 1 && cmd <= len(plugins)) {
        BrowserPluginEntry& e = plugins[cmd - 1];
        if (e.kind && str::Eq(e.kind, StrL("host")) && e.action) {
            RunHostPluginAction(win, e.action);
        } else if (e.kind && (str::Eq(e.kind, StrL("webview2-overlay")) || str::Eq(e.kind, StrL("webview2"))) &&
                   e.chromeId) {
            // Open extension options inside center browser — do NOT add to library.
            TempStr page = e.optionsPage ? e.optionsPage : StrL("index.html");
            TempStr url = fmt("chrome-extension://%s/%s", e.chromeId, page);
            WebBrowserOpenUrlAsNewTab(win, url, e.name ? e.name : StrL("Trancy"));
        } else if (e.folder) {
            SumatraOpenPathInDefaultFileManager(e.folder);
        }
    }
    for (BrowserPluginEntry& e : plugins) {
        FreeBrowserPluginEntry(&e);
    }
}

void OnWebPanelExtensionsButton(MainWindow* win) {
    ShowBrowserExtensionsMenu(win, StrL("Browser-AIChat"));
}

void OnWebBrowserExtensionsButton(MainWindow* win) {
    ShowBrowserExtensionsMenu(win, StrL("Browser-Library"));
}

void OnNotebookLmTabButton(MainWindow* win) {
    if (!win) {
        return;
    }
    win->uiState.aiChatVisible = false;
    win->uiState.webPanelVisible = true;
    EnsureWebPanelWebView(win);
    RestorePdfNotebookLmTab(win);
    ScheduleUiUpdate(win);
    WriteBridgeJson(win);
}

void OnWebPanelJsNotify(void* ctx, Str method, Str paramsJson) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !method) {
        return;
    }
    if (str::Eq(method, "docTitle")) {
        Str title = {};
        auto grab = [](Str* out, json::Value* v) {
            if (v->type == json::Type::String && json::PathMatch(v->path, StrL("i0"))) {
                *out = str::Dup(v->value);
                v->stop = true;
            }
        };
        json::Parse(paramsJson, MkFunc1<Str, json::Value*>(grab, &title));
        if (title && win->webPanelWebView) {
            TempStr url = win->webPanelWebView->GetSourceTemp();
            SyncWebPanelTabFromWebView(win, win->webPanelWebView, url, title);
        }
        str::Free(title);
    }
}

void CloseWebPanelFromLabel(MainWindow* win) {
    CloseWebPanel(win);
}

bool OnWebNavStarting(void* ctx, Str url, bool newWindow) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win)) {
        return true;
    }
    if (newWindow && url) {
        // AI panel only: popup / target=_blank → new AI tab.
        CreateNewWebPanelTab(win, url, TitleFromUrlTemp(url));
        return false;
    }
    return true;
}

// Center Web: never route into AI. Extension/site popups become browser tabs only —
// never auto-add to the library (Trancy etc. spam login/marketing windows otherwise).
bool OnWebBrowserNavStarting(void* ctx, Str url, bool newWindow) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win)) {
        return true;
    }
    if (newWindow && url) {
        if (str::EqI(url, StrL("about:blank")) || str::StartsWithI(url, StrL("about:blank?"))) {
            return false;
        }
        // Reuse an existing tab with the same URL to avoid popup storms.
        for (int i = 0; i < len(win->webBrowserTabs); i++) {
            if (win->webBrowserTabs[i].url && str::EqI(win->webBrowserTabs[i].url, url)) {
                ActivateWebBrowserTabByIndex(win, i, true);
                WebBrowserShowPanel(win);
                return false;
            }
        }
        WebBrowserOpenUrlAsNewTab(win, url, {});
        return false;
    }
    return true;
}

void OnWebNavCompleted(void* ctx, Str url, bool success) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !success) {
        return;
    }
    WebviewWnd* wv = win->webPanelWebView;
    TempStr title = wv ? wv->GetDocumentTitleTemp() : TempStr{};
    SyncWebPanelTabFromWebView(win, wv, url, title);
    if (!win->uiState.webPanelVisible) {
        return;
    }
    if (win->webPanelWebView) {
        win->webPanelWebView->SetControllerVisible(true);
        if (win->webPanelWebView->emulateMobile) {
            win->webPanelWebView->ApplyMobileEmulation();
        }
    }
    RelayoutWebPanel(win);
    WriteBridgeJson(win);
}

int FindWebPanelTabByWebView(MainWindow* win, WebviewWnd* wv) {
    if (!win || !wv) {
        return -1;
    }
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        if (win->webPanelTabs[i].wv == wv) {
            return i;
        }
    }
    return -1;
}

void SyncWebPanelTabFromWebView(MainWindow* win, WebviewWnd* wv, Str url, Str title) {
    if (!IsMainWindowValid(win)) {
        return;
    }
    int idx = FindWebPanelTabByWebView(win, wv);
    if (idx < 0 || idx >= len(win->webPanelTabs)) {
        // Browser-surface WebViews must not fall through to the AI active tab.
        return;
    }
    WebPanelTab& t = win->webPanelTabs[idx];
    bool changed = false;
    if (url && !str::StartsWithI(url, StrL("about:"))) {
        if (!str::Eq(t.url, url)) {
            str::ReplaceWithCopy(&t.url, url);
            changed = true;
        }
        if (idx == win->webPanelActiveTab) {
            str::ReplaceWithCopy(&win->webPanelCurrentUrl, url);
            str::ReplaceWithCopy(&gLastUrl, url);
        }
    }
    if (title && title.len > 0) {
        // Prefer the live document.title (Chrome tab label); skip empty / URL-looking fallbacks.
        // After a user rename, titleLocked freezes the label.
        bool titleIsUrl = str::StartsWithI(title, StrL("http://")) || str::StartsWithI(title, StrL("https://"));
        if (!t.titleLocked && !titleIsUrl && !str::Eq(t.title, title)) {
            str::ReplaceWithCopy(&t.title, title);
            changed = true;
        }
    }
    if (changed) {
        SaveWebPanelTabs(win);
        if (idx == win->webPanelActiveTab) {
            RememberPdfActiveTab(win);
            WriteBridgeJson(win);
        }
    }
}

void OnWebSourceChanged(void* ctx, WebviewWnd* sender, Str url) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !sender) {
        return;
    }
    TempStr title = sender->GetDocumentTitleTemp();
    SyncWebPanelTabFromWebView(win, sender, url, title);
}

void OnWebDocumentTitleChanged(void* ctx, WebviewWnd* sender, Str title) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !sender) {
        return;
    }
    TempStr url = sender->GetSourceTemp();
    SyncWebPanelTabFromWebView(win, sender, url, title);
}

void EnsureWebPanelWebView(MainWindow* win) {
    if (!win || !HasWebView()) {
        return;
    }
    LoadBookmarks();
    LoadWebPanelTabs(win);
    if (win->webPanelWebViewReady && win->webPanelWebView) {
        ShowActiveWebPanelTab(win);
        return;
    }
    if (win->webPanelActiveTab >= 0 && win->webPanelActiveTab < len(win->webPanelTabs)) {
        ActivateWebPanelTabByIndex(win, win->webPanelActiveTab, false);
        return;
    }
    if (len(win->webPanelTabs) > 0) {
        ActivateWebPanelTabByIndex(win, 0, false);
        return;
    }
    TempStr url = gLastUrl;
    if (!url || str::EqI(url, StrL("about:blank")) || str::EqI(url, StrL("about:blank/"))) {
        url = Str(kDefaultBookmarks[0].url);
    }
    CreateNewWebPanelTab(win, url, TitleFromUrlTemp(url));
}

LRESULT CALLBACK WndProcWebPanelBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) {
        return CallWindowProcW(gWebPanelBoxWndProc, hwnd, msg, wp, lp);
    }
    LRESULT res = 0;
    res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }
    if (VirtHostOnMessage(hwnd, win->webPanelRoot, msg, wp, lp, res, ThemeControlBackgroundColor())) {
        return res;
    }
    if (msg == WM_SIZE) {
        LayoutWebPanelBox(win);
        return 0;
    }
    return CallWindowProcW(gWebPanelBoxWndProc, hwnd, msg, wp, lp);
}

static const char* kIconRefresh =
    R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16"><path fill="currentColor" d="M13.65 2.35A7.95 7.95 0 0 0 8 0a8 8 0 1 0 7.5 10h-1.6A6.4 6.4 0 1 1 8 1.6c1.5 0 2.9.5 4 1.4L9.5 5.5H16V0l-2.35 2.35z"/></svg>)";

VirtIconButton* HeaderIconButton(const char* svg, Str tooltip, const VirtMouseHandler& onClick) {
    int sz = DpiScale(kPinIconPx);
    auto* button = new VirtIconButton();
    button->pixmap = GetCachedPixmapForSvg(Str(svg), sz, sz);
    int pad = DpiScale(2);
    button->padding = Insets{0, pad, 0, pad};
    button->onClick = onClick;
    button->SetTooltip(tooltip);
    return button;
}

} // namespace

void WebPanelClearPdfTabIds(i64 bookId) {
    UpsertPdfTabBinding(bookId, {}, {}, {}, {}, false, false, true, false);
}

void WebPanelClearPdfTabUrls(i64 bookId) {
    UpsertPdfTabBinding(bookId, {}, {}, {}, {}, false, false, false, true);
}

void WebPanelShowAiTabBindings(MainWindow* win, i64 bookId) {
    if (bookId <= 0) {
        return;
    }
    HWND parent = win ? win->hwndFrame : nullptr;
    PdfTabBinding bind = LoadPdfTabBindingForBook(bookId);
    LibraryBook* book = LibraryIsAvailable() ? LibraryStoreFindBookById(LibraryGetStore(), bookId) : nullptr;
    if (book && book->kind == LibraryBookKind::Web) {
        MigrateLegacyWebBrowserBinding(&bind, bookId);
    }

    str::Builder out;
    out.Append(fmt("bookId:           %lld\n", bookId));
    out.Append(fmt("kind:             %s\n",
                   book && book->kind == LibraryBookKind::Web ? StrL("web") : StrL("pdf")));
    out.Append(fmt("title:            %s\n", book && book->title ? book->title : StrL("")));
    out.Append(StrL("\n=== AI 面板 · 当前 Tab ===\n"));
    out.Append(fmt("webTabId:         %s\n", bind.webTabId ? bind.webTabId : StrL("(empty)")));
    out.Append(fmt("webTabUrl:        %s\n", bind.webTabUrl ? bind.webTabUrl : StrL("(empty)")));
    out.Append(StrL("\n=== AI 面板 · NotebookLM Tab ===\n"));
    out.Append(fmt("notebookTabId:    %s\n", bind.notebookTabId ? bind.notebookTabId : StrL("(empty)")));
    out.Append(fmt("notebookTabUrl:   %s\n", bind.notebookTabUrl ? bind.notebookTabUrl : StrL("(empty)")));
    if (book && book->kind == LibraryBookKind::Web) {
        out.Append(StrL("\n=== 中间网页 · Browser Tab ===\n"));
        out.Append(fmt("browserTabId:     %s\n", bind.browserTabId ? bind.browserTabId : StrL("(empty)")));
        out.Append(fmt("browserTabUrl:    %s\n", bind.browserTabUrl ? bind.browserTabUrl : StrL("(empty)")));
    }
    out.Append(StrL("\n文件: %OneDrive%\\SumatraPDF\\WebPanel\\tabs\\pdf-map.json\n"));
    DeleteLibraryBook(book);

    constexpr int kBtnClearIds = 1001;
    constexpr int kBtnClearUrls = 1002;
    struct DlgState {
        Str text;
    } state;
    state.text = str::Dup(ToStr(out));

#pragma pack(push, 1)
    struct {
        DLGTEMPLATE dlg;
        WORD menu;
        WORD cls;
        WCHAR title[1];
    } tmpl{};
#pragma pack(pop)
    tmpl.dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
    tmpl.dlg.dwExtendedStyle = WS_EX_DLGMODALFRAME;
    tmpl.dlg.cx = 420;
    tmpl.dlg.cy = 260;

    auto proc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR {
        auto* st = (DlgState*)GetWindowLongPtrW(hwnd, DWLP_USER);
        if (msg == WM_INITDIALOG) {
            st = (DlgState*)lp;
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)st);
            SetWindowTextW(hwnd, CWStrTemp(_TRA("伴随 AI Tab 页")));
            HFONT font = GetAppFont()->GetHFont();
            Rect rc = HwndClientRect(hwnd);
            HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, CWStrTemp(st->text),
                                       WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                           ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                       10, 10, rc.dx - 20, rc.dy - 52, hwnd, (HMENU)1000, GetModuleHandleW(nullptr),
                                       nullptr);
            HWND b1 = CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("删除 Tab ID")), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 10,
                                   rc.dy - 34, 100, 26, hwnd, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);
            HWND b2 = CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("删除 Tab URL")), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 118,
                                   rc.dy - 34, 110, 26, hwnd, (HMENU)1002, GetModuleHandleW(nullptr), nullptr);
            HWND b3 = CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("关闭")),
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, rc.dx - 88, rc.dy - 34, 78, 26,
                                   hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
            for (HWND c : {edit, b1, b2, b3}) {
                SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
            }
            return TRUE;
        }
        if (msg == WM_COMMAND) {
            int id = LOWORD(wp);
            if (id == 1001 || id == 1002) {
                EndDialog(hwnd, id);
                return TRUE;
            }
            if (id == IDCANCEL || id == IDOK) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
        }
        if (msg == WM_CLOSE) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    };

    INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tmpl.dlg, parent, proc, (LPARAM)&state);
    str::Free(state.text);
    FreePdfTabBinding(&bind);

    if (result == kBtnClearIds) {
        if (MessageBoxW(parent, CWStrTemp(_TRA("确定删除该条目记录的全部 Tab ID？")), CWStrTemp(_TRA("删除 Tab ID")),
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            WebPanelClearPdfTabIds(bookId);
        }
    } else if (result == kBtnClearUrls) {
        if (MessageBoxW(parent, CWStrTemp(_TRA("确定删除该条目记录的全部 Tab URL？")), CWStrTemp(_TRA("删除 Tab URL")),
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            WebPanelClearPdfTabUrls(bookId);
        }
    }
}

void RelayoutWebPanel(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox) {
        return;
    }
    LayoutWebPanelBox(win);
    if (win->webPanelWebView && win->webPanelWebViewReady) {
        win->webPanelWebView->UpdateWebviewSize();
    }
    RedrawWindow(win->hwndWebPanelBox, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void WebPanelPersistActiveBookAiState(MainWindow* win) {
    if (!win || win->activeLibraryBookId <= 0) {
        return;
    }
    if (win->uiState.webPanelVisible) {
        RememberPdfActiveTab(win);
    }
    SetBookAiPanelOpen(win->activeLibraryBookId, win->uiState.webPanelVisible);
    LibrarySaveUiState(win);
}

void DeferredApplyLastBookAi(MainWindow* win) {
    if (!IsMainWindowValid(win)) {
        return;
    }
    // Session may finish loading the PDF after CreateWebPanel; resolve book from
    // the current tab path when lastBookId was not restored yet.
    if (win->activeLibraryBookId <= 0 && LibraryIsAvailable()) {
        WindowTab* tab = win->CurrentTab();
        if (tab && tab->filePath) {
            LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
            if (book) {
                win->activeLibraryBookId = book->id;
                win->activeLibraryBookKind = (int)book->kind;
                DeleteLibraryBook(book);
            }
        }
    }
    if (win->activeLibraryBookId <= 0) {
        return;
    }
    if (win->libraryAiStateReady && win->aiAppliedBookId == win->activeLibraryBookId) {
        // already applied by LibraryOnActiveBookChanged; restoring bindings again
        // re-activates the same WebView tabs (~100 ms each)
        return;
    }
    LibraryBookKind kind = (LibraryBookKind)win->activeLibraryBookKind;
    if (LibraryIsAvailable()) {
        LibraryBook* book = LibraryStoreFindBookById(LibraryGetStore(), win->activeLibraryBookId);
        if (book) {
            kind = book->kind;
            win->activeLibraryBookKind = (int)kind;
            DeleteLibraryBook(book);
        }
    }
    ApplyBookAiPanelVisibility(win, win->activeLibraryBookId, kind);
    win->uiState.layout = {};
    ScheduleUiUpdate(win, kUiRelayout | kUiNoToolbars);
}

constexpr UINT_PTR kApplyBookAiTimerId = 0x41494150;

static void CALLBACK OnApplyBookAiTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (win && win->hwndFrame == hwnd) {
        DeferredApplyLastBookAi(win);
    }
}

// Restoring a book's AI tabs activates WebViews (100-300 ms). WM_TIMER is only
// delivered when no input is pending, so fast library/tab switching applies it
// once, for the book the user stops on.
void ScheduleApplyBookAi(MainWindow* win) {
    if (!win || !win->hwndFrame) {
        return;
    }
    // re-arming resets the delay: applies once the user stays on a book
    constexpr UINT kSettleMs = 250;
    SetTimer(win->hwndFrame, kApplyBookAiTimerId, kSettleMs, OnApplyBookAiTimer);
}

void CreateWebPanel(MainWindow* win) {
    if (!HasWebView()) {
        return;
    }
    EnsureWebPanelDataLayout();
    EnsureBrowserExtensionsLayout();
    EnsureScriptsWebviewLayout();
    UpdateWebPanelCpuSample();
    LoadBookmarks();

    int dx = gGlobalPrefs->aiChatSidebarDx > 0 ? gGlobalPrefs->aiChatSidebarDx : 360;
    win->webPanelDx = dx;
    DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    win->hwndWebPanelBox = CreateWindowExW(0, WC_STATICW, L"", style, 0, 0, dx, 0, win->hwndFrame, nullptr,
                                           GetModuleHandleW(nullptr), nullptr);

    PlatformFont* labelFont = GetAppSidebarLabelFont();
    auto header = NewLabelWithClose(win->hwndWebPanelBox, labelFont, MkFunc0(CloseWebPanelFromLabel, win));
    win->webPanelLabel = header.label;
    header.label->SetText(_TRA("AI"));
    win->webPanelBookmarksBtn = HeaderIconButton(gIconBookmarks, _TRA("书签（点击新建 Tab）"),
                                                 MkFunc0(OnBookmarksButton, win));
    win->webPanelTabsBtn = HeaderIconButton(gIconTabs, _TRA("Tab 列表"), MkFunc0(OnTabsButton, win));
    win->webPanelHistoryBtn =
        HeaderIconButton(gIconHistory, _TRA("历史记录"), MkFunc0(OnWebPanelHistoryButton, win));
    win->webPanelExtensionsBtn =
        HeaderIconButton(gIconExtensions, _TRA("扩展程序"), MkFunc0(OnWebPanelExtensionsButton, win));
    win->webPanelNotebookLmBtn =
        HeaderIconButton(gIconNotebookLm, _TRA("当前 PDF 的 NotebookLM Tab"), MkFunc0(OnNotebookLmTabButton, win));
    win->webPanelFocusPdfBtn =
        HeaderIconButton(gIconTargetFocus, _TRA("仅与当前 PDF 对话"), MkFunc0(OnFocusCurrentPdfNotebookLm, win));
    win->webPanelRefreshBtn = HeaderIconButton(kIconRefresh, _TRA("Refresh"), MkFunc0(OnWebPanelRefresh, win));

    // AI | bookmarks | tabs | history | extensions | notebooklm | focus-pdf | (spacer) | refresh | close
    if (len(header.box->children) > 0) {
        header.box->children[0].flex = 0;
        header.box->children.Pop();
    }
    header.box->AddChild(win->webPanelBookmarksBtn);
    header.box->AddChild(win->webPanelTabsBtn);
    header.box->AddChild(win->webPanelHistoryBtn);
    header.box->AddChild(win->webPanelExtensionsBtn);
    header.box->AddChild(win->webPanelNotebookLmBtn);
    header.box->AddChild(win->webPanelFocusPdfBtn);
    header.box->AddChild(new Spacer(0, 0), 1);
    header.box->AddChild(win->webPanelRefreshBtn);
    header.box->AddChild(header.closeBtn);
    win->webPanelHeader = header.box;

    auto* sep = new VirtLine();
    sep->thickness = 1;
    // use theme edge (same as Library / Bookmarks separators), not pure black

    win->webPanelWebView = nullptr;
    win->webPanelWebViewReady = false;
    win->webPanelActiveTab = -1;
    win->webPanelWebViewSlot = new Spacer(0, 0);

    auto* vbox = new VBox();
    vbox->alignCross = CrossAxisAlign::Stretch;
    vbox->AddChild(win->webPanelHeader);
    vbox->AddChild(sep);
    vbox->AddChild(win->webPanelWebViewSlot, 1);
    win->webPanelLayout = vbox;

    if (!gWebPanelBoxWndProc) {
        gWebPanelBoxWndProc = (WNDPROC)GetWindowLongPtrW(win->hwndWebPanelBox, GWLP_WNDPROC);
    }
    SetWindowLongPtrW(win->hwndWebPanelBox, GWLP_WNDPROC, (LONG_PTR)WndProcWebPanelBox);
    UpdateWebPanelTheme(win);

    if (win->webPanelSplitter) {
        win->webPanelSplitter->SetIsVisible(false);
        win->webPanelSplitter->onMove = MkFunc1Void(OnWebPanelSplitterMove);
    }

    // Per-book AI open/closed is restored after layout (lastBookId / session doc).
    win->uiState.webPanelVisible = false;
    win->uiState.aiChatVisible = false;
    win->aiAppliedBookId = 0;
    uitask::Post(MkFunc0(DeferredApplyLastBookAi, win), "ApplyLastBookAi");
}

void DestroyWebPanel(MainWindow* win) {
    if (!win) {
        return;
    }
    SaveWebPanelTabs(win);
    FreePinClickCtxs();
    for (WebPanelTab& tab : win->webPanelTabs) {
        delete tab.wv;
        tab.wv = nullptr;
        str::Free(tab.id);
        str::Free(tab.url);
        str::Free(tab.title);
        tab.id = {};
        tab.url = {};
        tab.title = {};
    }
    win->webPanelTabs.Reset();
    win->webPanelActiveTab = -1;
    win->webPanelWebView = nullptr;
    win->webPanelWebViewReady = false;
    delete win->webPanelLayout;
    win->webPanelLayout = nullptr;
    delete win->webPanelRoot;
    win->webPanelRoot = nullptr;
    win->webPanelHeader = nullptr;
    win->webPanelLabel = nullptr;
    win->webPanelBookmarksBtn = nullptr;
    win->webPanelTabsBtn = nullptr;
    win->webPanelHistoryBtn = nullptr;
    win->webPanelExtensionsBtn = nullptr;
    win->webPanelNotebookLmBtn = nullptr;
    win->webPanelFocusPdfBtn = nullptr;
    win->webPanelRefreshBtn = nullptr;
    win->webPanelWebViewSlot = nullptr;
    if (win->webPanelSplitter) {
        win->webPanelSplitter->onMove = {};
        win->webPanelSplitter->SetIsVisible(false);
    }
    str::Free(win->webPanelCurrentUrl);
    win->webPanelCurrentUrl = {};
    if (win->hwndWebPanelBox) {
        DestroyWindow(win->hwndWebPanelBox);
        win->hwndWebPanelBox = nullptr;
    }
}

void CloseWebPanel(MainWindow* win) {
    if (!win) {
        return;
    }
    if (win->activeLibraryBookId > 0) {
        SetBookAiPanelOpen(win->activeLibraryBookId, false);
    }
    win->uiState.webPanelVisible = false;
    // Drop WebView2 composition so it cannot steal space / paint over the center.
    for (int i = 0; i < len(win->webPanelTabs); i++) {
        WebviewWnd* wv = win->webPanelTabs[i].wv;
        if (!wv) {
            continue;
        }
        wv->SetControllerVisible(false, false);
        wv->SetIsVisible(false);
        if (wv->hwnd) {
            ShowWindow(wv->hwnd, SW_HIDE);
        }
    }
    if (win->hwndWebPanelBox) {
        HwndSetVisible(win->hwndWebPanelBox, false);
    }
    win->uiState.layout = {};
    ScheduleUiUpdate(win, kUiRelayout | kUiNoToolbars);
}

// Forward decls used by WebPanelOnDocumentChanged (defined below).
void WebPanelEnsureNotebookLmVisible(MainWindow* win);
void WebPanelSpawnScript(Str scriptName, Str extraArgs);

void OnWebPanelToggle(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox) {
        return;
    }
    if (win->uiState.webPanelVisible) {
        CloseWebPanel(win);
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (tab) {
        AIChatSetTabPanelOpen(tab, AIChatBackend::None);
    }
    AIChatSyncPanelsToCurrentTab(win);
    win->uiState.aiChatVisible = false;
    win->uiState.webPanelVisible = true;
    if (win->activeLibraryBookId > 0) {
        SetBookAiPanelOpen(win->activeLibraryBookId, true);
    }
    EnsureWebPanelWebView(win);
    if (win->activeLibraryBookId > 0) {
        RestoreBookAiBindings(win, win->activeLibraryBookId, (LibraryBookKind)win->activeLibraryBookKind);
    }
    ScheduleUiUpdate(win);
}

void LibraryOnActiveBookChanged(MainWindow* win, i64 bookId, int kind);
void OpenLibraryWebBook(MainWindow* win, i64 bookId);

void WebPanelOnDocumentChanged(MainWindow* win) {
    if (!win) {
        return;
    }
    // PDF tab switch: leave web browser surface and restore this book's AI bindings.
    WindowTab* tab = win->CurrentTab();
    if (tab && tab->filePath && LibraryIsAvailable()) {
        LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), tab->filePath);
        if (book) {
            LibraryOnActiveBookChanged(win, book->id, (int)book->kind);
            DeleteLibraryBook(book);
            return;
        }
    }
    win->uiState.webBrowserVisible = false;
    if (!win->uiState.webPanelVisible) {
        return;
    }
    WriteBridgeJson(win);
    RestorePdfActiveTab(win);
}

TempStr FindRepoScriptsWebviewTemp() {
    // SUMATRA_SCRIPTS overrides everything (folder that contains package.json).
    TempStr env = GetEnvVariableTemp(StrL("SUMATRA_SCRIPTS"));
    if (env && file::Exists(path::JoinTemp(env, StrL("package.json")))) {
        return env;
    }
    // Walk up from the EXE (out/dbg64 → repo root) looking for scripts/webview.
    TempStr dir = GetSelfExeDirTemp();
    for (int i = 0; i < 8 && dir; i++) {
        TempStr candidate = path::JoinTemp(dir, StrL("scripts\\webview"));
        if (file::Exists(path::JoinTemp(candidate, StrL("package.json")))) {
            return candidate;
        }
        TempStr parent = path::GetDirTemp(dir);
        if (!parent || str::EqI(parent, dir)) {
            break;
        }
        dir = parent;
    }
    return {};
}

static void CopyScriptsWebviewFile(Str dstRoot, Str srcRoot, Str rel) {
    TempStr src = path::JoinTemp(srcRoot, rel);
    TempStr dst = path::JoinTemp(dstRoot, rel);
    if (!file::Exists(src)) {
        return;
    }
    dir::CreateAll(path::GetDirTemp(dst));
    // Always refresh from repo so forks stay in sync with sources.
    file::Copy(dst, src, false);
}

void EnsureScriptsWebviewLayout() {
    // Copies ~15 files (into OneDrive by default) and is reached from many UI paths.
    static bool synced = false;
    if (synced) {
        return;
    }
    synced = true;
    TempStr src = FindRepoScriptsWebviewTemp();
    if (!src) {
        return;
    }
    TempStr dstRoot = path::JoinTemp(GetAppDataDirTemp(), StrL("scripts\\webview"));
    dir::CreateAll(dstRoot);
    dir::CreateAll(path::JoinTemp(dstRoot, StrL("lib")));
    dir::CreateAll(path::JoinTemp(dstRoot, StrL("manager")));

    // Source files only — never copy node_modules (forks run npm ci locally).
    static const char* kFiles[] = {
        "package.json",
        "package-lock.json",
        "config.json",
        "manifest.json",
        "README.md",
        "cli.mjs",
        "flywheel.mjs",
        "watch-jobs.mjs",
        "notebooklm-add.mjs",
        "notebooklm-select.mjs",
        "notebooklm-chat.mjs",
        "lib/bridge.mjs",
        "lib/config.mjs",
        "lib/notebooklm.mjs",
        "manager/editor.html",
    };
    for (const char* rel : kFiles) {
        CopyScriptsWebviewFile(dstRoot, src, Str(rel));
    }
    if (!file::Exists(path::JoinTemp(dstRoot, StrL("node_modules\\playwright\\package.json")))) {
        logf("EnsureScriptsWebviewLayout: synced scripts to '%s' — run: cd that folder && npm ci\n", dstRoot);
    }
}

TempStr ScriptsWebviewDirTemp() {
    EnsureScriptsWebviewLayout();

    // 1) Explicit override
    TempStr env = GetEnvVariableTemp(StrL("SUMATRA_SCRIPTS"));
    if (env && file::Exists(path::JoinTemp(env, StrL("package.json")))) {
        return env;
    }
    // 2) Synced appdata copy (OneDrive or portable) — preferred when npm-installed
    TempStr app = path::JoinTemp(GetAppDataDirTemp(), StrL("scripts\\webview"));
    if (file::Exists(path::JoinTemp(app, StrL("package.json")))) {
        return app;
    }
    // 3) Repo next to the build tree (developer checkout)
    TempStr repo = FindRepoScriptsWebviewTemp();
    if (repo) {
        return repo;
    }
    return app;
}

TempStr FindNodeExeTemp() {
    TempStr envNode = GetEnvVariableTemp(StrL("SUMATRA_NODE"));
    if (envNode && file::Exists(envNode)) {
        return envNode;
    }
    TempStr candidates[] = {
        path::JoinTemp(GetEnvVariableTemp(StrL("ProgramFiles")), StrL("nodejs\\node.exe")),
        path::JoinTemp(GetEnvVariableTemp(StrL("LOCALAPPDATA")), StrL("Programs\\node\\node.exe")),
    };
    for (TempStr c : candidates) {
        if (c && file::Exists(c)) {
            return c;
        }
    }
#ifdef _MSC_VER
    WCHAR pathW[MAX_PATH];
    if (SearchPathW(nullptr, L"node.exe", nullptr, MAX_PATH, pathW, nullptr) > 0) {
        return ToUtf8Temp(pathW);
    }
#endif
    return {};
}

void NotebookLmPollLoopThread() {
    for (int i = 0; i < 90; i++) {
        Sleep(2000);
        uitask::Post(MkFunc0Void(WebPanelPollBridgeResults), "NotebookLmPoll");
    }
}

void ScheduleNotebookLmResultPolls() {
    // Playwright uploads can take minutes; keep polling done/ without a delay API.
    RunAsync(MkFunc0Void(NotebookLmPollLoopThread), "NotebookLmPollLoop");
}

void WebPanelEnsureNotebookLmVisible(MainWindow* win) {
    if (!win) {
        return;
    }
    win->uiState.aiChatVisible = false;
    win->uiState.webPanelVisible = true;
    EnsureWebPanelWebView(win);
    // Prefer this PDF's remembered NotebookLM tab (stable id → url fallback).
    RestorePdfNotebookLmTab(win);
    ScheduleUiUpdate(win);
    WriteBridgeJson(win);
}

void WebPanelSpawnScript(Str scriptName, Str extraArgs) {
    TempStr dir = ScriptsWebviewDirTemp();
    TempStr script = path::JoinTemp(dir, scriptName);
    if (!file::Exists(script)) {
        logf("WebPanelSpawnScript: missing '%s'\n", script);
        return;
    }
    TempStr node = FindNodeExeTemp();
    if (!node || !file::Exists(node)) {
        logf("WebPanelSpawnScript: node.exe not found (set SUMATRA_NODE). script='%s'\n", script);
        return;
    }
    TempStr cmd =
        extraArgs ? fmt("\"%s\" \"%s\" %s", node, script, extraArgs) : fmt("\"%s\" \"%s\"", node, script);
    // Hide node.exe console (CREATE_NO_WINDOW + SW_HIDE + NUL stdio).
    HANDLE h = LaunchProcessHidden(cmd, dir);
    if (!h) {
        logf("WebPanelSpawnScript: CreateProcess failed node='%s' cmd='%s'\n", node, cmd);
        return;
    }
    CloseHandle(h);
    logf("WebPanelSpawnScript: ok (hidden) node='%s' script='%s'\n", node, scriptName);
}

void WebPanelAddToNotebookLm(MainWindow* win, i64 bookId, Str pdfPath, Str sourceUrl, Str title) {
    if (!pdfPath && !sourceUrl) {
        return;
    }
    if (win) {
        WebPanelEnsureNotebookLmVisible(win);
    }
    TempStr jobs = WebPanelJobsPendingTemp();
    dir::CreateAll(jobs);
    dir::CreateAll(WebPanelJobsDoneTemp());
    dir::CreateAll(WebPanelJobsFailedTemp());

    TempStr priorJson = {};
    if (bookId > 0) {
        Str blob = LibraryStoreGetBookNotebookLm(LibraryGetStore(), bookId);
        if (blob) {
            priorJson = str::DupTemp(blob);
            str::Free(blob);
        }
    }

    TempStr jobPath = path::JoinTemp(jobs, fmt("add-%lld.json", (i64)UnixTimeMsNow()));
    TempStr body = fmt(
        "{\n  \"action\": \"notebooklm.add\",\n  \"bookId\": %lld,\n  \"pdfPath\": %s,\n  \"sourceUrl\": %s,\n"
        "  \"title\": %s,\n  \"notebooklm\": %s\n}\n",
        bookId, EscapeJsonTemp(pdfPath), EscapeJsonTemp(sourceUrl), EscapeJsonTemp(title),
        priorJson ? EscapeJsonTemp(priorJson) : StrL("\"\""));
    file::WriteFile(jobPath, body);
    TempStr args = fmt("--job \"%s\"", jobPath);
    WebPanelSpawnScript(StrL("notebooklm-add.mjs"), args);
    ScheduleNotebookLmResultPolls();
    logf("WebPanelAddToNotebookLm: queued '%s' pdf='%s' url='%s'\n", jobPath, pdfPath ? pdfPath : StrL(""),
         sourceUrl ? sourceUrl : StrL(""));
}

void WebPanelAddPdfToNotebookLm(MainWindow* win, i64 bookId, Str pdfPath, Str title) {
    WebPanelAddToNotebookLm(win, bookId, pdfPath, {}, title);
}

void WebPanelSelectNotebookLmSource(MainWindow* win, i64 bookId, Str pdfPath, Str title) {
    if (!pdfPath) {
        return;
    }
    if (win) {
        WebPanelEnsureNotebookLmVisible(win);
    }
    TempStr nbUrl = {};
    TempStr json = {};
    if (bookId > 0) {
        Str blob = LibraryStoreGetBookNotebookLm(LibraryGetStore(), bookId);
        if (blob) {
            json = str::DupTemp(blob);
            str::Free(blob);
            Str key = StrL("\"notebookUrl\"");
            int k = str::IndexOfI(json, key);
            if (k >= 0) {
                Str rest = Str(json.s + k + key.len, json.len - k - key.len);
                int q1 = str::IndexOfChar(rest, '"');
                if (q1 >= 0) {
                    Str after = Str(rest.s + q1 + 1, rest.len - q1 - 1);
                    int q2 = str::IndexOfChar(after, '"');
                    if (q2 > 0) {
                        nbUrl = str::DupTemp(Str(after.s, q2));
                    }
                }
            }
        }
    }
    TempStr sourceTitle = title && title.len > 0 ? title : path::GetBaseNameTemp(pdfPath);
    TempStr jobs = WebPanelJobsPendingTemp();
    dir::CreateAll(jobs);
    TempStr jobPath = path::JoinTemp(jobs, fmt("select-%lld.json", (i64)UnixTimeMsNow()));
    TempStr body = fmt(
        "{\n  \"action\": \"notebooklm.select\",\n  \"bookId\": %lld,\n  \"pdfPath\": %s,\n"
        "  \"fileName\": %s,\n  \"sourceTitle\": %s,\n  \"notebookUrl\": %s,\n  \"notebooklm\": %s\n}\n",
        bookId, EscapeJsonTemp(pdfPath), EscapeJsonTemp(sourceTitle), EscapeJsonTemp(sourceTitle),
        EscapeJsonTemp(nbUrl), EscapeJsonTemp(json));
    file::WriteFile(jobPath, body);
    TempStr args = fmt("--job \"%s\"", jobPath);
    WebPanelSpawnScript(StrL("notebooklm-select.mjs"), args);
    logf("WebPanelSelectNotebookLmSource: queued '%s'\n", jobPath);
}

TempStr WebPanelDbgControlTemp(Str action, Str a, Str b, int n1, int n2, int* exitCodeOut) {
    (void)n2;
    str::Builder out;
    auto finish = [&](Str msg, int code) -> TempStr {
        if (msg) {
            out.Append(msg);
        }
        if (!str::EndsWith(ToStr(out), StrL("\n"))) {
            out.AppendChar('\n');
        }
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return ToStrTemp(out);
    };

    if (len(gWindows) == 0) {
        return finish(StrL("NOTREADY no-window"), 2);
    }
    MainWindow* win = gWindows[0];
    if (!win) {
        return finish(StrL("NOTREADY no-window"), 2);
    }

    if (!action || str::EqI(action, "status")) {
        TempStr url = win->webPanelCurrentUrl ? win->webPanelCurrentUrl : gLastUrl;
        int port = win->webPanelCdpPort > 0 ? win->webPanelCdpPort : gCdpPort;
        return finish(fmt("OK visible=%d tabs=%d cdp=%d url=%s bridge=%s", win->uiState.webPanelVisible ? 1 : 0,
                          len(win->webPanelTabs), port, url ? url : StrL(""), BridgePathTemp()),
                      0);
    }

    if (str::EqI(action, "show")) {
        WebPanelEnsureNotebookLmVisible(win);
        return finish(fmt("OK visible=1 cdp=%d", win->webPanelCdpPort > 0 ? win->webPanelCdpPort : gCdpPort), 0);
    }

    if (str::EqI(action, "hide")) {
        CloseWebPanel(win);
        return finish(StrL("OK visible=0"), 0);
    }

    if (str::EqI(action, "poll")) {
        WebPanelPollBridgeResults();
        return finish(StrL("OK polled"), 0);
    }

    if (str::EqI(action, "add")) {
        // a=pdfPath, n1=bookId (optional), b=title (optional)
        if (!a) {
            return finish(StrL("ERROR add expects pdfPath [bookId] [title]"), 1);
        }
        i64 bookId = n1 > 0 ? n1 : 0;
        if (bookId <= 0) {
            LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), a);
            if (book) {
                bookId = book->id;
                DeleteLibraryBook(book);
            }
        }
        TempStr title = b && b.len > 0 ? b : path::GetBaseNameTemp(a);
        WebPanelAddPdfToNotebookLm(win, bookId, a, title);
        return finish(fmt("OK queued bookId=%lld path=%s", bookId, a), 0);
    }

    if (str::EqI(action, "notebooklm")) {
        // a=pdfPath optional, n1=bookId optional; else current tab path
        TempStr path = a;
        if ((!path || path.len == 0) && n1 <= 0) {
            WindowTab* tab = win->CurrentTab();
            if (tab && tab->filePath) {
                path = tab->filePath;
            }
        }
        if (!path || path.len == 0) {
            return finish(StrL("ERROR notebooklm expects path or current document"), 1);
        }
        LibraryBook* book = LibraryStoreFindBookByPath(LibraryGetStore(), path);
        if (!book) {
            return finish(StrL("ERROR no-book"), 1);
        }
        TempStr json = book->notebooklm ? book->notebooklm : StrL("");
        i64 id = book->id;
        DeleteLibraryBook(book);
        return finish(fmt("OK bookId=%lld notebooklm=%s", id, json), 0);
    }

    return finish(fmt("ERROR unknown-action %s", action ? action : StrL("")), 1);
}

void WebPanelPollBridgeResults() {
    UpdateWebPanelCpuSample();
    TempStr doneDir = WebPanelJobsDoneTemp();
    if (!dir::Exists(doneDir)) {
        return;
    }
    DirIter di{doneDir};
    for (DirIterEntry* e : di) {
        if (!e || e->isDir || !str::EndsWithI(e->name, StrL(".json"))) {
            continue;
        }
        TempStr path = e->filePath ? e->filePath : path::JoinTemp(doneDir, e->name);
        Str data = file::ReadFile(path);
        if (!data) {
            continue;
        }
        auto grab = [](Str json, Str key) -> TempStr {
            int k = str::IndexOfI(json, key);
            if (k < 0) {
                return {};
            }
            Str rest = Str(json.s + k + key.len, json.len - k - key.len);
            int q1 = str::IndexOfChar(rest, '"');
            if (q1 < 0) {
                return {};
            }
            Str after = Str(rest.s + q1 + 1, rest.len - q1 - 1);
            int q2 = str::IndexOfChar(after, '"');
            if (q2 <= 0) {
                return {};
            }
            return str::DupTemp(Str(after.s, q2));
        };
        i64 bookId = 0;
        {
            int k = str::IndexOfI(data, StrL("\"bookId\""));
            if (k >= 0) {
                Str rest = Str(data.s + k, data.len - k);
                int colon = str::IndexOfChar(rest, ':');
                if (colon >= 0) {
                    bookId = atoi(rest.s + colon + 1);
                }
            }
        }
        TempStr notebook = grab(data, StrL("\"notebook\""));
        TempStr notebookUrl = grab(data, StrL("\"notebookUrl\""));
        TempStr sourceTitle = grab(data, StrL("\"sourceTitle\""));
        bool okFlag = str::ContainsI(data, StrL("\"ok\": true")) || str::ContainsI(data, StrL("\"ok\":true")) ||
                      (notebook && notebookUrl);
        if (okFlag && bookId > 0 && notebook) {
            TempStr json = fmt(
                "{\"notebook\":%s,\"notebookUrl\":%s,\"sourceTitle\":%s,\"updatedMs\":%lld}",
                EscapeJsonTemp(notebook), EscapeJsonTemp(notebookUrl), EscapeJsonTemp(sourceTitle),
                (i64)UnixTimeMsNow());
            LibraryStoreSetBookNotebookLm(LibraryGetStore(), bookId, json);
            logf("WebPanelPollBridgeResults: book %lld -> %s\n", bookId, notebook);
        }
        str::Free(data);
        file::Delete(path);
    }
}

void UpdateWebPanelDpi(MainWindow* win, int dpi) {
    if (!win || !win->hwndWebPanelBox || dpi <= 0) {
        return;
    }
    if (win->webPanelLabel) {
        win->webPanelLabel->font = GetAppSidebarLabelFontForDpi(dpi);
    }
    RelayoutWebPanel(win);
}

static void SetHeaderIcon(VirtIconButton* b, const char* svg) {
    if (!b) {
        return;
    }
    int sz = DpiScale(kPinIconPx);
    b->pixmap = GetCachedPixmapForSvg(Str(svg), sz, sz);
}

void UpdateWebPanelIcons(MainWindow* win) {
    if (!win) {
        return;
    }
    SetHeaderIcon(win->webPanelBookmarksBtn, gIconBookmarks);
    SetHeaderIcon(win->webPanelTabsBtn, gIconTabs);
    SetHeaderIcon(win->webPanelHistoryBtn, gIconHistory);
    SetHeaderIcon(win->webPanelExtensionsBtn, gIconExtensions);
    SetHeaderIcon(win->webPanelNotebookLmBtn, gIconNotebookLm);
    SetHeaderIcon(win->webPanelFocusPdfBtn, gIconTargetFocus);
    SetHeaderIcon(win->webPanelRefreshBtn, kIconRefresh);
    SetHeaderIcon(win->webBrowserBookmarksBtn, gIconBookmarks);
    SetHeaderIcon(win->webBrowserTabsBtn, gIconTabs);
    SetHeaderIcon(win->webBrowserHistoryBtn, gIconHistory);
    SetHeaderIcon(win->webBrowserExtensionsBtn, gIconExtensions);
    SetHeaderIcon(win->webBrowserRefreshBtn, kIconRefresh);
    if (win->hwndWebPanelBox) {
        HwndInvalidate(win->hwndWebPanelBox);
    }
    if (win->hwndWebBrowserBox) {
        HwndInvalidate(win->hwndWebBrowserBox);
    }
}

void UpdateWebPanelTheme(MainWindow* win) {
    if (!win || !win->hwndWebPanelBox) {
        return;
    }
    DarkModeApplyToChildControls(win->hwndWebPanelBox);
    RedrawWindow(win->hwndWebPanelBox, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

bool IsWebPanelVisible(MainWindow* win) {
    return win && win->uiState.webPanelVisible;
}

// --- Center Web browser panel (library web books; isolated tabs, shared env/CDP) ---

namespace {

TempStr WebBrowserTabsPathTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("tabs\\web-index.json"));
}

TempStr WebBookmarksPathTemp() {
    return path::JoinTemp(WebPanelDataDirTemp(), StrL("web-bookmarks.txt"));
}

WNDPROC gWebBrowserBoxWndProc = nullptr;

void ShowActiveWebBrowserTab(MainWindow* win);
void SaveWebBrowserTabs(MainWindow* win);
void LoadWebBrowserTabs(MainWindow* win);
void ActivateWebBrowserTabByIndex(MainWindow* win, int idx, bool remember);
void RememberBrowserActiveTab(MainWindow* win);
void SyncWebBrowserTabFromWebView(MainWindow* win, WebviewWnd* wv, Str url, Str title);

int FindWebBrowserTabById(MainWindow* win, Str id) {
    if (!win || !id) {
        return -1;
    }
    for (int i = 0; i < len(win->webBrowserTabs); i++) {
        if (str::Eq(win->webBrowserTabs[i].id, id)) {
            return i;
        }
    }
    return -1;
}

int FindWebBrowserTabByWebView(MainWindow* win, WebviewWnd* wv) {
    if (!win || !wv) {
        return -1;
    }
    for (int i = 0; i < len(win->webBrowserTabs); i++) {
        if (win->webBrowserTabs[i].wv == wv) {
            return i;
        }
    }
    return -1;
}

void SaveWebBrowserTabs(MainWindow* win) {
    if (!win) {
        return;
    }
    EnsureWebPanelDataLayout();
    str::Builder sb;
    sb.Append(StrL("{\n  \"activeId\": "));
    Str activeId = {};
    if (win->webBrowserActiveTab >= 0 && win->webBrowserActiveTab < len(win->webBrowserTabs)) {
        activeId = win->webBrowserTabs[win->webBrowserActiveTab].id;
    }
    sb.Append(EscapeJsonTemp(activeId));
    sb.Append(StrL(",\n  \"tabs\": [\n"));
    for (int i = 0; i < len(win->webBrowserTabs); i++) {
        WebPanelTab& t = win->webBrowserTabs[i];
        if (i > 0) {
            sb.Append(StrL(",\n"));
        }
        sb.Append(StrL("    {\"id\": "));
        sb.Append(EscapeJsonTemp(t.id));
        sb.Append(StrL(", \"url\": "));
        sb.Append(EscapeJsonTemp(t.url));
        sb.Append(StrL(", \"title\": "));
        sb.Append(EscapeJsonTemp(t.title ? t.title : TitleFromUrlTemp(t.url)));
        if (t.titleLocked) {
            sb.Append(StrL(", \"titleLocked\": true"));
        }
        sb.Append(StrL("}"));
    }
    sb.Append(StrL("\n  ]\n}\n"));
    file::WriteFile(WebBrowserTabsPathTemp(), ToStr(sb));
}

void LoadWebBrowserTabs(MainWindow* win) {
    if (!win || len(win->webBrowserTabs) > 0) {
        return;
    }
    Str data = file::ReadFile(WebBrowserTabsPathTemp());
    if (!data) {
        return;
    }
    struct St {
        MainWindow* win = nullptr;
        WebPanelTab cur{};
        bool inTab = false;
        Str activeId;
    } st;
    st.win = win;
    auto onVal = [](St* s, json::Value* v) {
        TempStr p = json::PathFormatTemp(v->path);
        if (!p) {
            return;
        }
        if (str::EqI(p, StrL("/activeId"))) {
            str::ReplaceWithCopy(&s->activeId, v->value);
            return;
        }
        if (!str::ContainsI(p, StrL("/tabs"))) {
            return;
        }
        if (str::EndsWithI(p, StrL("/id"))) {
            if (s->inTab && s->cur.url) {
                if (!s->cur.id) {
                    s->cur.id = str::Dup(NewWebPanelTabIdTemp());
                }
                if (!s->cur.title) {
                    s->cur.title = str::Dup(TitleFromUrlTemp(s->cur.url));
                }
                s->win->webBrowserTabs.Append(s->cur);
                s->cur = {};
            }
            s->inTab = true;
            str::ReplaceWithCopy(&s->cur.id, v->value);
        } else if (str::EndsWithI(p, StrL("/url"))) {
            s->inTab = true;
            str::ReplaceWithCopy(&s->cur.url, v->value);
        } else if (str::EndsWithI(p, StrL("/title"))) {
            s->inTab = true;
            str::ReplaceWithCopy(&s->cur.title, v->value);
        } else if (str::EndsWithI(p, StrL("/titleLocked"))) {
            s->inTab = true;
            s->cur.titleLocked = v->value && (str::EqI(v->value, StrL("true")) || str::Eq(v->value, StrL("1")));
        }
    };
    json::Parse(data, MkFunc1<St, json::Value*>(onVal, &st));
    if (st.inTab && st.cur.url) {
        if (!st.cur.id) {
            st.cur.id = str::Dup(NewWebPanelTabIdTemp());
        }
        if (!st.cur.title) {
            st.cur.title = str::Dup(TitleFromUrlTemp(st.cur.url));
        }
        win->webBrowserTabs.Append(st.cur);
    }
    if (st.activeId) {
        for (int i = 0; i < len(win->webBrowserTabs); i++) {
            if (str::Eq(win->webBrowserTabs[i].id, st.activeId)) {
                win->webBrowserActiveTab = i;
                break;
            }
        }
    }
    str::Free(st.activeId);
    str::Free(data);
}

void RememberBrowserActiveTab(MainWindow* win) {
    if (!win || win->webBrowserActiveTab < 0 || win->webBrowserActiveTab >= len(win->webBrowserTabs)) {
        return;
    }
    if (!LibraryIsAvailable() || win->activeLibraryBookId <= 0) {
        return;
    }
    if ((LibraryBookKind)win->activeLibraryBookKind != LibraryBookKind::Web) {
        return;
    }
    WebPanelTab& t = win->webBrowserTabs[win->webBrowserActiveTab];
    // Center browser binding — do not overwrite AI webTab*.
    UpsertPdfTabBinding(win->activeLibraryBookId, {}, {}, {}, {}, false, false, false, false, t.id, t.url, true);
    SaveWebBrowserTabs(win);
}

void ShowActiveWebBrowserTab(MainWindow* win) {
    if (!win || !win->webBrowserWebViewSlot) {
        return;
    }
    // Panel hidden → every browser controller must be off or WebView2 will
    // keep compositing over the PDF canvas (ghost / overlap on fast switch).
    if (!win->uiState.webBrowserVisible) {
        for (int i = 0; i < len(win->webBrowserTabs); i++) {
            WebviewWnd* wv = win->webBrowserTabs[i].wv;
            if (!wv) {
                continue;
            }
            wv->SetControllerVisible(false, false);
            wv->SetIsVisible(false);
            if (wv->hwnd) {
                ShowWindow(wv->hwnd, SW_HIDE);
            }
        }
        return;
    }
    Rect wr = win->webBrowserWebViewSlot->lastBounds;
    if (wr.dx < 1) {
        wr.dx = 1;
    }
    if (wr.dy < 1) {
        wr.dy = 1;
    }
    for (int i = 0; i < len(win->webBrowserTabs); i++) {
        WebviewWnd* wv = win->webBrowserTabs[i].wv;
        if (!wv || !wv->hwnd) {
            continue;
        }
        bool active = (i == win->webBrowserActiveTab);
        if (active) {
            MoveWindow(wv->hwnd, wr.x, wr.y, wr.dx, wr.dy, TRUE);
            wv->EnsureOpaqueBackground();
            wv->SetIsVisible(true);
            wv->SetControllerVisible(true, false);
            wv->UpdateWebviewSize();
            win->webBrowserWebView = wv;
        } else {
            wv->SetControllerVisible(false, false);
            wv->SetIsVisible(false);
            ShowWindow(wv->hwnd, SW_HIDE);
        }
    }
}

void LayoutWebBrowserBox(MainWindow* win) {
    if (!win || !win->hwndWebBrowserBox || !win->webBrowserLayout) {
        return;
    }
    Rect rc = HwndClientRect(win->hwndWebBrowserBox);
    LayoutTreeToSize(win->hwndWebBrowserBox, win->webBrowserLayout, {rc.dx, rc.dy}, &win->webBrowserRoot);
    ShowActiveWebBrowserTab(win);
}

void OnWebBrowserSourceChanged(void* ctx, WebviewWnd* sender, Str url) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !sender) {
        return;
    }
    SyncWebBrowserTabFromWebView(win, sender, url, sender->GetDocumentTitleTemp());
}

void OnWebBrowserDocumentTitleChanged(void* ctx, WebviewWnd* sender, Str title) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !sender) {
        return;
    }
    SyncWebBrowserTabFromWebView(win, sender, sender->GetSourceTemp(), title);
}

void OnWebBrowserNavCompleted(void* ctx, Str url, bool success) {
    auto* win = (MainWindow*)ctx;
    if (!IsMainWindowValid(win) || !success) {
        return;
    }
    WebviewWnd* wv = win->webBrowserWebView;
    SyncWebBrowserTabFromWebView(win, wv, url, wv ? wv->GetDocumentTitleTemp() : TempStr{});
    if (!win->uiState.webBrowserVisible) {
        // Switched back to PDF before navigate finished — do not resurface WebView2.
        ShowActiveWebBrowserTab(win);
        return;
    }
    if (win->webBrowserWebView) {
        // allowSuspend=false: navigate-complete must not fight PDF/Web XOR hide/show.
        win->webBrowserWebView->SetControllerVisible(true, false);
        if (win->webBrowserWebView->emulateMobile) {
            win->webBrowserWebView->ApplyMobileEmulation();
        }
    }
    LayoutWebBrowserBox(win);
}

static i64 FindBookIdByBrowserTabId(Str tabId) {
    if (!tabId) {
        return 0;
    }
    Vec<PdfTabBinding> all = LoadAllPdfTabBindings();
    i64 found = 0;
    for (PdfTabBinding& b : all) {
        if ((b.browserTabId && str::Eq(b.browserTabId, tabId)) || (b.webTabId && str::Eq(b.webTabId, tabId))) {
            found = ParseInt64(b.bookKey);
            break;
        }
    }
    FreeAllPdfTabBindings(all);
    return found;
}

void SyncWebBrowserTabFromWebView(MainWindow* win, WebviewWnd* wv, Str url, Str title) {
    if (!IsMainWindowValid(win)) {
        return;
    }
    int idx = FindWebBrowserTabByWebView(win, wv);
    if (idx < 0 || idx >= len(win->webBrowserTabs)) {
        return;
    }
    WebPanelTab& t = win->webBrowserTabs[idx];
    bool changed = false;

    // Resolve which library web book owns this browser tab (Chrome-like 1:1).
    i64 bookId = FindBookIdByBrowserTabId(t.id);
    if (bookId <= 0 && idx == win->webBrowserActiveTab && win->activeLibraryBookId > 0 &&
        (LibraryBookKind)win->activeLibraryBookKind == LibraryBookKind::Web) {
        bookId = win->activeLibraryBookId;
    }

    if (url && !str::StartsWithI(url, StrL("about:"))) {
        if (!str::Eq(t.url, url)) {
            str::ReplaceWithCopy(&t.url, url);
            changed = true;
        }
        if (idx == win->webBrowserActiveTab) {
            str::ReplaceWithCopy(&win->webBrowserCurrentUrl, url);
        }
        if (LibraryIsAvailable() && bookId > 0) {
            LibraryBook* book = LibraryStoreFindBookById(LibraryGetStore(), bookId);
            if (book && book->kind == LibraryBookKind::Web && (!book->url || !str::Eq(book->url, url))) {
                LibraryStoreSetBookUrl(LibraryGetStore(), bookId, url);
            }
            DeleteLibraryBook(book);
        }
    }
    if (title && title.len > 0) {
        bool titleIsUrl = str::StartsWithI(title, StrL("http://")) || str::StartsWithI(title, StrL("https://"));
        // Skip URL-looking titles; real document.title drives the library row.
        if (!titleIsUrl) {
            if (!str::Eq(t.title, title)) {
                str::ReplaceWithCopy(&t.title, title);
                t.titleLocked = false;
                changed = true;
            }
            // Always push to library even when tab title was already in sync
            // (web-index may have the title while books.title is still a URL).
            if (LibraryIsAvailable() && bookId > 0) {
                LibraryUpdateWebBookTabTitle(bookId, title);
            }
        }
    }
    if (changed) {
        SaveWebBrowserTabs(win);
        if (idx == win->webBrowserActiveTab) {
            RememberBrowserActiveTab(win);
        }
    }
}

WebviewWnd* CreateWebBrowserTabWebView(MainWindow* win, Str url) {
    if (!win || !url || !HasWebView() || !win->hwndWebBrowserBox) {
        return nullptr;
    }
    EnsureWebPanelDataLayout();
    dir::CreateAll(WebViewBrowserProfileDirTemp());

    auto* webView = new WebviewWnd();
    webView->events.ctx = win;
    // Dedicated handler — must NOT share OnWebNavStarting (that opens AI tabs).
    webView->events.navigationStarting = OnWebBrowserNavStarting;
    webView->events.navigationCompleted = OnWebBrowserNavCompleted;
    webView->events.sourceChanged = OnWebBrowserSourceChanged;
    webView->events.documentTitleChanged = OnWebBrowserDocumentTitleChanged;
    webView->dataDir = str::Dup(WebViewBrowserProfileDirTemp());
    webView->useDedicatedEnvironment = true;
    webView->enableDevTools = true;
    // Opaque white — default transparent background lets the PDF canvas show through.
    webView->defaultBackgroundColor = kColWhite;
    // Full desktop browsing (no mobile UA / touch emulation). Mobile mode
    // breaks slide captchas and overlay close clicks; keep mobile on AI panel only.
    // Spoof Chrome (not Edge) so Gemini / Google AI pages use the correct fonts.
    webView->emulateMobile = false;
    webView->userAgent = str::Dup(kChromeDesktopUserAgent);
    webView->dedicatedBrowserArgs = str::Dup(fmt("--remote-debugging-port=%d", kLibraryCdpPort));
    webView->enableBrowserExtensions = true;
    // Chrome overlays (Trancy, …) — LocalAppData, not git / not OneDrive.
    webView->browserExtensionsDir = str::Dup(BrowserExtensionsOverlayDirTemp());
    webView->allowClipboardRead = true;
    webView->desiredVisible = false;

    CreateWebViewArgs wvArgs;
    wvArgs.parent = win->hwndWebBrowserBox;
    wvArgs.pos = Rect(0, 0, 1, 1);
    webView->Create(wvArgs);
    if (!webView->hwnd) {
        delete webView;
        return nullptr;
    }
    ShowWindow(webView->hwnd, SW_HIDE);
    webView->Navigate(url);
    return webView;
}

void ActivateWebBrowserTabByIndex(MainWindow* win, int idx, bool remember) {
    if (!win || idx < 0 || idx >= len(win->webBrowserTabs)) {
        return;
    }
    WebPanelTab& t = win->webBrowserTabs[idx];
    if (!t.wv && t.url) {
        t.wv = CreateWebBrowserTabWebView(win, t.url);
        if (!t.wv) {
            return;
        }
    }
    win->webBrowserActiveTab = idx;
    if (t.url) {
        str::ReplaceWithCopy(&win->webBrowserCurrentUrl, t.url);
    }
    win->webBrowserWebView = t.wv;
    win->webBrowserWebViewReady = t.wv != nullptr;
    ShowActiveWebBrowserTab(win);
    SaveWebBrowserTabs(win);
    LayoutWebBrowserBox(win);
    if (remember) {
        RememberBrowserActiveTab(win);
    }
}

void CreateNewWebBrowserTab(MainWindow* win, Str url, Str title, Str forcedId = {}) {
    if (!win || !url) {
        return;
    }
    if (forcedId) {
        int existing = FindWebBrowserTabById(win, forcedId);
        if (existing >= 0) {
            ActivateWebBrowserTabByIndex(win, existing, true);
            return;
        }
    }
    WebviewWnd* wv = CreateWebBrowserTabWebView(win, url);
    if (!wv) {
        return;
    }
    WebPanelTab tab;
    tab.id = str::Dup(forcedId && forcedId.len > 0 ? forcedId : NewWebPanelTabIdTemp());
    tab.url = str::Dup(url);
    tab.title = str::Dup(title && title.len > 0 ? title : url);
    tab.wv = wv;
    win->webBrowserTabs.Append(tab);
    ActivateWebBrowserTabByIndex(win, len(win->webBrowserTabs) - 1, true);
}

bool ActivateOrRecreateWebBrowserTab(MainWindow* win, Str tabId, Str tabUrl, Str titleFallback) {
    if (!win) {
        return false;
    }
    if (tabId) {
        int idx = FindWebBrowserTabById(win, tabId);
        if (idx >= 0) {
            ActivateWebBrowserTabByIndex(win, idx, true);
            return true;
        }
    }
    if (tabUrl && tabUrl.len > 0) {
        CreateNewWebBrowserTab(win, tabUrl, titleFallback && titleFallback.len > 0 ? titleFallback : tabUrl, tabId);
        return true;
    }
    return false;
}

void CloseWebBrowserTabAt(MainWindow* win, int idx) {
    if (!win || idx < 0 || idx >= len(win->webBrowserTabs)) {
        return;
    }
    WebPanelTab& t = win->webBrowserTabs[idx];
    delete t.wv;
    str::Free(t.id);
    str::Free(t.url);
    str::Free(t.title);
    win->webBrowserTabs.RemoveAt(idx);
    if (win->webBrowserActiveTab == idx) {
        win->webBrowserActiveTab = -1;
        win->webBrowserWebView = nullptr;
        win->webBrowserWebViewReady = false;
        if (len(win->webBrowserTabs) > 0) {
            int next = idx < len(win->webBrowserTabs) ? idx : len(win->webBrowserTabs) - 1;
            ActivateWebBrowserTabByIndex(win, next, true);
        } else {
            // No center Web tabs left — fall back to PDF canvas.
            win->uiState.webBrowserVisible = false;
            ApplyCenterContentSurface(win);
            win->uiState.layout = {};
            ScheduleUiUpdate(win, kUiRelayout | kUiNoToolbars);
        }
    } else if (win->webBrowserActiveTab > idx) {
        win->webBrowserActiveTab--;
    }
    SaveWebBrowserTabs(win);
    ShowActiveWebBrowserTab(win);
    LayoutWebBrowserBox(win);
}

enum {
    kWebBrowserMenuCloseAll = 9001,
    kWebBrowserMenuRenameCurrent = 9002,
};

static Str PromptWebBrowserText(HWND parent, Str title, Str label, Str initial) {
    // Minimal single-line prompt (same pattern as library rename).
    struct St {
        Str title;
        Str label;
        Str initial;
        HWND edit = nullptr;
        Str result;
    } state{title, label, initial};
    enum { kEditId = 1001 };
#pragma pack(push, 2)
    struct Tpl {
        DLGTEMPLATE dlg{};
        WORD menu = 0;
        WORD windowClass = 0;
        WCHAR titleW = 0;
    } t;
#pragma pack(pop)
    t.dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    t.dlg.dwExtendedStyle = WS_EX_DLGMODALFRAME;
    t.dlg.cx = 280;
    t.dlg.cy = 92;
    auto proc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR {
        auto* s = (St*)GetWindowLongPtrW(hwnd, DWLP_USER);
        if (msg == WM_INITDIALOG) {
            s = (St*)lp;
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)s);
            SetWindowTextW(hwnd, CWStrTemp(s->title));
            HFONT font = GetAppFont()->GetHFont();
            Rect rc = HwndClientRect(hwnd);
            HWND lab = CreateWindowW(WC_STATICW, CWStrTemp(s->label), WS_CHILD | WS_VISIBLE, 12, 12, rc.dx - 24, 20,
                                     hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            s->edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, CWStrTemp(s->initial ? s->initial : Str{}),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 35, rc.dx - 24, 24, hwnd,
                                      (HMENU)kEditId, GetModuleHandleW(nullptr), nullptr);
            HWND ok = CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("OK")), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                   rc.dx - 174, rc.dy - 38, 76, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
            HWND cancel = CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("Cancel")), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                        rc.dx - 88, rc.dy - 38, 76, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr),
                                        nullptr);
            for (HWND c : {lab, s->edit, ok, cancel}) {
                SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
            }
            HwndSetFocus(s->edit);
            SendMessageW(s->edit, EM_SETSEL, 0, -1);
            return FALSE;
        }
        if (msg == WM_COMMAND && LOWORD(wp) == IDOK && s) {
            int n = GetWindowTextLengthW(s->edit);
            WCHAR* value = AllocArrayTemp<WCHAR>(n + 1);
            GetWindowTextW(s->edit, value, n + 1);
            s->result = str::Dup(ToUtf8Temp(value));
            str::TrimWSInPlace(s->result, str::TrimOpt::Both);
            if (!s->result) {
                MessageBeep(MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if ((msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) || msg == WM_CLOSE) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    };
    INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &t.dlg, parent, proc, (LPARAM)&state);
    if (result != IDOK) {
        str::Free(state.result);
        return {};
    }
    return state.result;
}

void ShowWebBrowserTabsMenu(MainWindow* win) {
    if (!win || !win->hwndWebBrowserBox) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    for (int i = 0; i < len(win->webBrowserTabs); i++) {
        WebPanelTab& t = win->webBrowserTabs[i];
        TempStr label = t.title ? t.title : (t.url ? t.url : StrL("(blank)"));
        UINT flags = MF_STRING;
        if (i == win->webBrowserActiveTab) {
            flags |= MF_CHECKED;
        }
        AppendMenuW(menu, flags, (UINT)(i + 1), CWStrTemp(label));
    }
    if (len(win->webBrowserTabs) > 0) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kWebBrowserMenuCloseAll, CWStrTemp(_TRA("关闭全部 Tab")));
    }
    POINT pt{};
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    DestroyMenu(menu);
    if (cmd == kWebBrowserMenuCloseAll) {
        for (int i = len(win->webBrowserTabs) - 1; i >= 0; i--) {
            CloseWebBrowserTabAt(win, i);
        }
        return;
    }
    if (cmd >= 1 && cmd <= len(win->webBrowserTabs)) {
        ActivateWebBrowserTabByIndex(win, cmd - 1, true);
    }
}

void OnWebBrowserBookmarks(MainWindow* win) {
    if (!win) {
        return;
    }
    LoadBookmarks();
    HMENU menu = CreatePopupMenu();
    for (int i = 0; i < len(gBookmarks); i++) {
        TempStr label = gBookmarks[i].title ? gBookmarks[i].title : gBookmarks[i].url;
        AppendMenuW(menu, MF_STRING, (UINT)(i + 1), CWStrTemp(label));
    }
    if (len(gBookmarks) == 0) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, CWStrTemp(_TRA("(无书签)")));
    }
    POINT pt{};
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    DestroyMenu(menu);
    if (cmd >= 1 && cmd <= len(gBookmarks)) {
        CreateNewWebBrowserTab(win, gBookmarks[cmd - 1].url, gBookmarks[cmd - 1].title);
    }
}

void OnWebBrowserTabs(MainWindow* win) {
    ShowWebBrowserTabsMenu(win);
}

void OnWebBrowserHistoryButton(MainWindow* win) {
    if (!win) {
        return;
    }
    WebBrowserShowPanel(win);
    CreateNewWebBrowserTab(win, StrL("edge://history/"), _TRA("历史记录"));
}

void OnWebBrowserRefresh(MainWindow* win) {
    if (!win) {
        return;
    }
    ShowActiveWebBrowserTab(win);
    if (win->webBrowserWebView) {
        win->webBrowserWebView->Reload(true);
    }
}

void CloseWebBrowserFromLabel(MainWindow* win) {
    if (!win) {
        return;
    }
    win->uiState.webBrowserVisible = false;
    ScheduleUiUpdate(win);
}

LRESULT CALLBACK WndProcWebBrowserBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) {
        return CallWindowProcW(gWebBrowserBoxWndProc, hwnd, msg, wp, lp);
    }
    LRESULT res = 0;
    res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }
    if (VirtHostOnMessage(hwnd, win->webBrowserRoot, msg, wp, lp, res, ThemeControlBackgroundColor())) {
        return res;
    }
    if (msg == WM_SIZE) {
        LayoutWebBrowserBox(win);
        return 0;
    }
    return CallWindowProcW(gWebBrowserBoxWndProc, hwnd, msg, wp, lp);
}

void EnsureWebBrowserWebView(MainWindow* win) {
    if (!win || !HasWebView()) {
        return;
    }
    LoadWebBrowserTabs(win);
    if (win->webBrowserWebViewReady && win->webBrowserWebView) {
        ShowActiveWebBrowserTab(win);
        return;
    }
    if (win->webBrowserActiveTab >= 0 && win->webBrowserActiveTab < len(win->webBrowserTabs)) {
        ActivateWebBrowserTabByIndex(win, win->webBrowserActiveTab, false);
        return;
    }
    if (len(win->webBrowserTabs) > 0) {
        ActivateWebBrowserTabByIndex(win, 0, false);
    }
}

void DeferredEnsureWebBrowserWebView(MainWindow* win) {
    if (!IsMainWindowValid(win) || !win->uiState.webBrowserVisible) {
        return;
    }
    EnsureWebBrowserWebView(win);
}

} // namespace

static void ClearPdfBrowserTabBinding(i64 bookId) {
    if (bookId <= 0) {
        return;
    }
    TempStr key = fmt("%lld", bookId);
    Vec<PdfTabBinding> all = LoadAllPdfTabBindings();
    PdfTabBinding* b = FindPdfTabBinding(&all, key);
    if (b) {
        str::Free(b->browserTabId);
        str::Free(b->browserTabUrl);
        b->browserTabId = {};
        b->browserTabUrl = {};
        SaveAllPdfTabBindings(all);
    }
    FreeAllPdfTabBindings(all);
}

void WebBrowserCloseTabForLibraryBook(MainWindow* win, i64 bookId) {
    if (!win || bookId <= 0) {
        return;
    }
    PdfTabBinding bind = LoadPdfTabBindingForBook(bookId);
    MigrateLegacyWebBrowserBinding(&bind, bookId);
    bool closed = false;
    if (bind.browserTabId) {
        int idx = FindWebBrowserTabById(win, bind.browserTabId);
        if (idx >= 0) {
            CloseWebBrowserTabAt(win, idx);
            closed = true;
        }
    }
    // Always close the visible center tab when this library book is active.
    if (!closed && win->activeLibraryBookId == bookId && win->webBrowserActiveTab >= 0 &&
        win->webBrowserActiveTab < len(win->webBrowserTabs)) {
        CloseWebBrowserTabAt(win, win->webBrowserActiveTab);
        closed = true;
    }
    if (!closed && bind.browserTabUrl) {
        for (int i = 0; i < len(win->webBrowserTabs); i++) {
            if (win->webBrowserTabs[i].url && str::EqI(win->webBrowserTabs[i].url, bind.browserTabUrl)) {
                CloseWebBrowserTabAt(win, i);
                break;
            }
        }
    }
    ClearPdfBrowserTabBinding(bookId);
    FreePdfTabBinding(&bind);
}

void CreateWebBrowserPanel(MainWindow* win) {
    if (!HasWebView() || !win) {
        return;
    }
    EnsureWebPanelDataLayout();
    DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    win->hwndWebBrowserBox = CreateWindowExW(0, WC_STATICW, L"", style, 0, 0, 100, 0, win->hwndFrame, nullptr,
                                             GetModuleHandleW(nullptr), nullptr);

    PlatformFont* labelFont = GetAppSidebarLabelFont();
    auto header = NewLabelWithClose(win->hwndWebBrowserBox, labelFont, MkFunc0(CloseWebBrowserFromLabel, win));
    win->webBrowserLabel = header.label;
    header.label->SetText(_TRA("Web"));
    win->webBrowserBookmarksBtn =
        HeaderIconButton(gIconBookmarks, _TRA("书签（点击新建 Tab）"), MkFunc0(OnWebBrowserBookmarks, win));
    win->webBrowserTabsBtn = HeaderIconButton(gIconTabs, _TRA("Tab 列表"), MkFunc0(OnWebBrowserTabs, win));
    win->webBrowserHistoryBtn =
        HeaderIconButton(gIconHistory, _TRA("历史记录"), MkFunc0(OnWebBrowserHistoryButton, win));
    win->webBrowserExtensionsBtn =
        HeaderIconButton(gIconExtensions, _TRA("扩展程序"), MkFunc0(OnWebBrowserExtensionsButton, win));
    win->webBrowserRefreshBtn = HeaderIconButton(kIconRefresh, _TRA("Refresh"), MkFunc0(OnWebBrowserRefresh, win));

    if (len(header.box->children) > 0) {
        header.box->children[0].flex = 0;
        header.box->children.Pop();
    }
    header.box->AddChild(win->webBrowserBookmarksBtn);
    header.box->AddChild(win->webBrowserTabsBtn);
    header.box->AddChild(win->webBrowserHistoryBtn);
    header.box->AddChild(win->webBrowserExtensionsBtn);
    header.box->AddChild(new Spacer(0, 0), 1);
    header.box->AddChild(win->webBrowserRefreshBtn);
    header.box->AddChild(header.closeBtn);
    win->webBrowserHeader = header.box;

    auto* sep = new VirtLine();
    sep->thickness = 1;
    win->webBrowserWebView = nullptr;
    win->webBrowserWebViewReady = false;
    win->webBrowserActiveTab = -1;
    win->webBrowserWebViewSlot = new Spacer(0, 0);

    auto* vbox = new VBox();
    vbox->alignCross = CrossAxisAlign::Stretch;
    vbox->AddChild(win->webBrowserHeader);
    vbox->AddChild(sep);
    vbox->AddChild(win->webBrowserWebViewSlot, 1);
    win->webBrowserLayout = vbox;

    if (!gWebBrowserBoxWndProc) {
        gWebBrowserBoxWndProc = (WNDPROC)GetWindowLongPtrW(win->hwndWebBrowserBox, GWLP_WNDPROC);
    }
    SetWindowLongPtrW(win->hwndWebBrowserBox, GWLP_WNDPROC, (LONG_PTR)WndProcWebBrowserBox);
    DarkModeApplyToChildControls(win->hwndWebBrowserBox);
    win->uiState.webBrowserVisible = false;
    HwndSetVisible(win->hwndWebBrowserBox, false);
}

void DestroyWebBrowserPanel(MainWindow* win) {
    if (!win) {
        return;
    }
    SaveWebBrowserTabs(win);
    for (WebPanelTab& tab : win->webBrowserTabs) {
        delete tab.wv;
        tab.wv = nullptr;
        str::Free(tab.id);
        str::Free(tab.url);
        str::Free(tab.title);
    }
    win->webBrowserTabs.Reset();
    win->webBrowserActiveTab = -1;
    win->webBrowserWebView = nullptr;
    win->webBrowserWebViewReady = false;
    delete win->webBrowserLayout;
    win->webBrowserLayout = nullptr;
    delete win->webBrowserRoot;
    win->webBrowserRoot = nullptr;
    win->webBrowserHeader = nullptr;
    win->webBrowserLabel = nullptr;
    win->webBrowserBookmarksBtn = nullptr;
    win->webBrowserTabsBtn = nullptr;
    win->webBrowserHistoryBtn = nullptr;
    win->webBrowserExtensionsBtn = nullptr;
    win->webBrowserRefreshBtn = nullptr;
    win->webBrowserWebViewSlot = nullptr;
    if (win->hwndWebBrowserBox) {
        DestroyWindow(win->hwndWebBrowserBox);
        win->hwndWebBrowserBox = nullptr;
    }
    win->uiState.webBrowserVisible = false;
}

void RelayoutWebBrowserPanel(MainWindow* win) {
    if (!win || !win->hwndWebBrowserBox) {
        return;
    }
    LayoutWebBrowserBox(win);
    if (win->webBrowserWebView && win->webBrowserWebViewReady) {
        win->webBrowserWebView->UpdateWebviewSize();
    }
    RedrawWindow(win->hwndWebBrowserBox, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

bool IsWebBrowserPanelVisible(MainWindow* win) {
    return win && win->uiState.webBrowserVisible;
}

void ApplyCenterContentSurface(MainWindow* win) {
    if (!win) {
        return;
    }
    WindowTab* cur = win->CurrentTab();
    bool favAsTab = cur && cur->IsFavoritesTab();
    bool wantWeb = !favAsTab && win->uiState.webBrowserVisible && win->hwndWebBrowserBox;
    logf("ApplyCenterContentSurface t=%llu wantWeb=%d browserVis=%d tabs=%d active=%d canvasHwnd=0x%p browserHwnd=0x%p\n",
         (u64)GetTickCount64(), wantWeb ? 1 : 0, win->uiState.webBrowserVisible ? 1 : 0, len(win->webBrowserTabs),
         win->webBrowserActiveTab, win->hwndCanvas, win->hwndWebBrowserBox);

    if (!wantWeb) {
        // PDF (or favorites): hide WebView2 composition without TrySuspend (fast XOR).
        for (int i = 0; i < len(win->webBrowserTabs); i++) {
            WebviewWnd* wv = win->webBrowserTabs[i].wv;
            if (!wv) {
                continue;
            }
            wv->SetControllerVisible(false, false);
            wv->SetIsVisible(false);
            if (wv->hwnd) {
                ShowWindow(wv->hwnd, SW_HIDE);
            }
        }
        if (win->hwndWebBrowserBox) {
            HwndSetVisible(win->hwndWebBrowserBox, false);
        }
        if (!favAsTab) {
            HwndSetVisible(win->hwndCanvas, true);
            SetWindowPos(win->hwndCanvas, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return;
    }

    // Web surface: hide PDF canvas first so a transparent WebView cannot show it through.
    HwndSetVisible(win->hwndCanvas, false);
    HwndSetVisible(win->hwndWebBrowserBox, true);
    SetWindowPos(win->hwndWebBrowserBox, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowActiveWebBrowserTab(win);
    if (win->hwndWebBrowserBox) {
        RelayoutWebBrowserPanel(win);
    }
}

void LibraryOnActiveBookChanged(MainWindow* win, i64 bookId, int kindInt) {
    if (!win || bookId <= 0) {
        return;
    }
    LibraryBookKind kind = kindInt == (int)LibraryBookKind::Web ? LibraryBookKind::Web : LibraryBookKind::Pdf;
    logf("LibraryOnActiveBookChanged t=%llu bookId=%lld kind=%s prevBrowserVis=%d\n", (u64)GetTickCount64(), bookId,
         kind == LibraryBookKind::Web ? StrL("web") : StrL("pdf"), win->uiState.webBrowserVisible ? 1 : 0);
    i64 prevBookId = win->activeLibraryBookId;
    // The AI panel only reflects prevBookId if its (deferred) apply ran; when the user
    // skipped past it, saving would store another book's panel state under it.
    bool panelShowsPrev = win->libraryAiStateReady && win->aiAppliedBookId == prevBookId;
    // Persist prior entry's AI visibility only after restore finished — otherwise
    // session tab switches wipe lastBookId's aiOpen=1 with a premature false.
    if (prevBookId > 0 && prevBookId != bookId && panelShowsPrev) {
        SetBookAiPanelOpen(prevBookId, win->uiState.webPanelVisible);
    }
    if (win->uiState.webPanelVisible && panelShowsPrev) {
        RememberPdfActiveTab(win);
    }
    if (win->uiState.webBrowserVisible &&
        (LibraryBookKind)win->activeLibraryBookKind == LibraryBookKind::Web) {
        RememberBrowserActiveTab(win);
    }
    win->activeLibraryBookId = bookId;
    win->activeLibraryBookKind = (int)kind;
    LibrarySaveUiState(win);

    if (kind == LibraryBookKind::Web) {
        win->uiState.webBrowserVisible = true;
        EnsureWebBrowserWebView(win);
        LibraryBook* book = LibraryStoreFindBookById(LibraryGetStore(), bookId);
        PdfTabBinding bind = LoadPdfTabBindingForBook(bookId);
        MigrateLegacyWebBrowserBinding(&bind, bookId);
        TempStr fallbackUrl = book && book->url ? book->url : Str{};
        TempStr fallbackTitle = book && book->title ? book->title : Str{};
        if (fallbackTitle && (str::StartsWithI(fallbackTitle, StrL("http://")) ||
                              str::StartsWithI(fallbackTitle, StrL("https://")))) {
            fallbackTitle = {}; // library shows full URL as fallback; tab waits for document.title
        }
        Str browserId = bind.browserTabId;
        Str browserUrl = bind.browserTabUrl ? bind.browserTabUrl : fallbackUrl;
        if (!ActivateOrRecreateWebBrowserTab(win, browserId, browserUrl, fallbackTitle)) {
            if (fallbackUrl) {
                CreateNewWebBrowserTab(win, fallbackUrl, fallbackTitle);
            }
        }
        if (win->webBrowserWebView) {
            SyncWebBrowserTabFromWebView(win, win->webBrowserWebView, win->webBrowserWebView->GetSourceTemp(),
                                         win->webBrowserWebView->GetDocumentTitleTemp());
        }
        LibraryStoreTouchBookOpen(LibraryGetStore(), bookId, UnixTimeMsNow());
        DeleteLibraryBook(book);
        FreePdfTabBinding(&bind);
        ApplyCenterContentSurface(win);
    } else {
        win->uiState.webBrowserVisible = false;
        ApplyCenterContentSurface(win); // hide WebView2 layers immediately
        if (win->uiState.webPanelVisible) {
            WriteBridgeJson(win);
        }
    }
    // Restore this book's AI open/closed + companion tabs (auto-load on startup / switch).
    ScheduleApplyBookAi(win);
    SyncLibrarySelection(win);
    win->uiState.layout = {}; // force RelayoutFrame to apply slot sizes
    ScheduleUiUpdate(win, kUiRelayout | kUiNoToolbars);
}

void OpenLibraryWebBook(MainWindow* win, i64 bookId) {
    if (!win || bookId <= 0 || !LibraryIsAvailable()) {
        return;
    }
    LibraryBook* book = LibraryStoreFindBookById(LibraryGetStore(), bookId);
    if (!book || book->kind != LibraryBookKind::Web) {
        DeleteLibraryBook(book);
        return;
    }
    DeleteLibraryBook(book);
    LibraryOnActiveBookChanged(win, bookId, (int)LibraryBookKind::Web);
}

void WebBrowserShowPanel(MainWindow* win) {
    if (!win) {
        return;
    }
    win->uiState.webBrowserVisible = true;
    EnsureWebBrowserWebView(win);
    ApplyCenterContentSurface(win);
    win->uiState.layout = {};
    ScheduleUiUpdate(win, kUiRelayout | kUiNoToolbars);
}

void WebBrowserOpenUrlAsNewTab(MainWindow* win, Str url, Str title) {
    if (!win || !url) {
        return;
    }
    WebBrowserShowPanel(win);
    // Always a new tab — duplicates allowed (do not pass forcedId).
    CreateNewWebBrowserTab(win, url, title && title.len > 0 ? title : url);
}
