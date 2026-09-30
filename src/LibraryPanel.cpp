/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/DirScan.h"
#include "base/File.h"
#include "base/Timer.h"
#include "base/Win.h"
#include "base/UITask.h"

#include "gui/Dpi.h"
#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/VirtCtrl.h"

#include "Settings.h"
#include "SumatraConfig.h"
#include "AppSettings.h"
#include "AppTools.h"
#include "DisplayMode.h"
#include "DocController.h"
#include "EngineBase.h"
#include "FileHistory.h"
#include "GlobalPrefs.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "Tabs.h"
#include "Library.h"
#include "LibraryStore.h"
#include "SvgIcons.h"
#include "Theme.h"
#include "Translations.h"
#include "WebPanel.h"
#include "PdfTools.h"
#include "ScriptManager.h"

#include "base/JsonParser.h"
#include "FilterHighlightDraw.h"
#include "LibraryPanel.h"
#include "SumatraLog.h"

enum class LibraryTreeKind {
    Root,
    Collection,
    Book,
    Error,
};

struct LibraryTreeItem {
    ~LibraryTreeItem();

    uintptr_t userData = 0;
    LibraryTreeItem* parent = nullptr;
    Vec<LibraryTreeItem*> children;
    LibraryTreeKind kind = LibraryTreeKind::Root;
    Str text;
    Str path;
    Str url;
    LibraryBookKind bookKind = LibraryBookKind::Pdf;
    i64 bookId = 0;
    i64 collectionId = 0;
    bool isShelf = false;
    bool expanded = false;
    u32 bgColor = 0;
};

LibraryTreeItem::~LibraryTreeItem() {
    DeleteVecMembers(children);
    str::Free(text);
    str::Free(path);
    str::Free(url);
}

struct LibraryTreeModel : TreeModel {
    ~LibraryTreeModel() override { delete root; }
    TreeItem Root() override { return (TreeItem)root; }
    Str Text(TreeItem item) override { return ((LibraryTreeItem*)item)->text; }
    TreeItem Parent(TreeItem item) override { return (TreeItem)((LibraryTreeItem*)item)->parent; }
    int ChildCount(TreeItem item) override { return len(((LibraryTreeItem*)item)->children); }
    TreeItem ChildAt(TreeItem item, int idx) override { return (TreeItem)((LibraryTreeItem*)item)->children[idx]; }
    bool IsExpanded(TreeItem item) override { return ((LibraryTreeItem*)item)->expanded; }
    bool IsChecked(TreeItem) override { return false; }
    void SetUserData(TreeItem item, uintptr_t data) override { ((LibraryTreeItem*)item)->userData = data; }
    uintptr_t GetUserData(TreeItem item) override { return ((LibraryTreeItem*)item)->userData; }
    i64 StableKey(TreeItem ti) override {
        auto* item = (LibraryTreeItem*)ti;
        switch (item->kind) {
            case LibraryTreeKind::Collection:
                return item->collectionId * 4 + 1;
            case LibraryTreeKind::Book:
                return item->bookId * 4 + 2;
            case LibraryTreeKind::Error:
                return 3;
            default:
                return 0;
        }
    }

    LibraryTreeItem* root = nullptr;
};

static LibraryTreeItem* NewItem(LibraryTreeItem* parent, LibraryTreeKind kind, Str text) {
    auto* item = new LibraryTreeItem();
    item->parent = parent;
    item->kind = kind;
    item->text = str::Dup(text);
    if (parent) parent->children.Append(item);
    return item;
}

static void AddBookItem(LibraryTreeItem* parent, LibraryBook* book) {
    TempStr label = book->title;
    if (book->kind == LibraryBookKind::Web) {
        Str url = book->url ? book->url : book->path;
        // Prefer stored tab title; fall back to full URL only until title syncs.
        bool titleIsUrl =
            label && (str::StartsWithI(label, StrL("http://")) || str::StartsWithI(label, StrL("https://")));
        if (!label || titleIsUrl) {
            label = url;
        }
        label = fmt("🌐 %s", label ? label : StrL("网页"));
    }
    auto* item = NewItem(parent, LibraryTreeKind::Book, label);
    item->bookId = book->id;
    item->path = book->path;
    item->url = book->url;
    item->bookKind = book->kind;
    item->bgColor = book->bgColor;
    // ownership moved to the tree item
    book->path = Str();
    book->url = Str();
}

struct CollectionIdIndex {
    i64 id;
    LibraryTreeItem* item;
};

static LibraryTreeItem* FindCollectionItem(Vec<CollectionIdIndex>& index, i64 id) {
    int lo = 0;
    int hi = len(index) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (index[mid].id == id) {
            return index[mid].item;
        }
        if (index[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return nullptr;
}

static void PruneEmptyCollections(LibraryTreeItem* parent) {
    for (int i = len(parent->children) - 1; i >= 0; i--) {
        LibraryTreeItem* child = parent->children[i];
        if (child->kind != LibraryTreeKind::Collection) continue;
        PruneEmptyCollections(child);
        if (len(child->children) == 0) {
            parent->children.RemoveAt(i);
            delete child;
        }
    }
}

static bool IsPdfTab(WindowTab* tab) {
    return tab && tab->filePath && str::EndsWithI(tab->filePath, StrL(".pdf"));
}

static LibraryTreeModel* BuildModel(MainWindow* win, Str filter) {
    auto* model = new LibraryTreeModel();
    model->root = NewItem(nullptr, LibraryTreeKind::Root, Str());
    if (!LibraryIsAvailable()) {
        TempStr msg = fmt("Library unavailable: %s", LibraryGetError());
        NewItem(model->root, LibraryTreeKind::Error, msg);
        return model;
    }

    Vec<LibraryCollection*> collections = LibraryStoreGetCollections(LibraryGetStore());
    Vec<LibraryTreeItem*> collectionItems;
    Vec<CollectionIdIndex> byId;
    for (LibraryCollection* collection : collections) {
        auto* item = new LibraryTreeItem();
        item->kind = LibraryTreeKind::Collection;
        item->text = str::Dup(collection->name);
        item->collectionId = collection->id;
        item->isShelf = collection->isShelf;
        item->bgColor = collection->bgColor;
        item->expanded =
            filter ? true
                   : (!win->libraryExpansionInitialized ? collection->isShelf
                                                        : win->expandedLibraryCollections.Contains(collection->id));
        collectionItems.Append(item);
        byId.Append({collection->id, item});
    }
    VecSort(byId, [](const CollectionIdIndex* a, const CollectionIdIndex* b) -> int {
        return (a->id > b->id) - (a->id < b->id);
    });
    // Phase 1: attach all folders first so siblings always list folders above PDFs.
    for (int i = 0; i < len(collections); i++) {
        LibraryCollection* collection = collections[i];
        LibraryTreeItem* item = collectionItems[i];
        LibraryTreeItem* parent = collection->parentId ? FindCollectionItem(byId, collection->parentId) : nullptr;
        if (!parent) {
            parent = model->root;
        }
        item->parent = parent;
        parent->children.Append(item);
    }
    DeleteLibraryCollections(collections);
    // Phase 2: append books under each folder / root (after all child folders).
    Vec<LibraryBook*> books = LibraryStoreGetPlacedBooks(LibraryGetStore(), filter);
    for (LibraryBook* book : books) {
        LibraryTreeItem* parent = book->placementId ? FindCollectionItem(byId, book->placementId) : model->root;
        if (parent) {
            AddBookItem(parent, book);
        }
    }
    // A broad filter ("a") matches most of a huge library; auto-expanding would insert every
    // row into the TreeView. Leave folders collapsed (lazily filled) until the filter narrows.
    constexpr int kFilterAutoExpandMaxBooks = 2000;
    if (filter && len(books) > kFilterAutoExpandMaxBooks) {
        for (LibraryTreeItem* item : collectionItems) {
            item->expanded = false;
        }
    }
    DeleteLibraryBooks(books);
    if (filter) {
        PruneEmptyCollections(model->root);
    }
    return model;
}

static TempStr FilterTextTemp(MainWindow* win) {
    return win && win->libraryFilterEdit ? win->libraryFilterEdit->GetTextTemp() : TempStr();
}

static void RememberExpandedCollectionsRec(MainWindow* win, LibraryTreeItem* item) {
    if (item->kind == LibraryTreeKind::Collection) {
        // lazily inserted tree: rows under a never-expanded folder have no handle, keep their model state
        TreeView* tv = win->libraryTreeView;
        bool expanded = tv->GetHandleByTreeItem((TreeItem)item) ? tv->IsExpanded((TreeItem)item) : item->expanded;
        if (expanded) {
            win->expandedLibraryCollections.Append(item->collectionId);
        }
    }
    for (LibraryTreeItem* child : item->children) {
        if (child->kind == LibraryTreeKind::Collection) {
            RememberExpandedCollectionsRec(win, child);
        }
    }
}

static TempStr LibraryTreeUiPathTemp() {
    return GetPathInAppDataDirTemp(StrL("SumatraPDF-library-ui.json"));
}

static void SaveLibraryTreeUiState(MainWindow* win) {
    if (!win) {
        return;
    }
    str::Builder out;
    out.Append(StrL("{\n  \"expandedCollections\": ["));
    for (int i = 0; i < len(win->expandedLibraryCollections); i++) {
        if (i > 0) {
            out.Append(StrL(", "));
        }
        out.Append(fmt("%lld", win->expandedLibraryCollections[i]));
    }
    out.Append(StrL("],\n  \"lastBookId\": "));
    out.Append(fmt("%lld", win->activeLibraryBookId > 0 ? win->activeLibraryBookId : (i64)0));
    out.Append(StrL("\n}\n"));
    file::WriteFile(LibraryTreeUiPathTemp(), ToStr(out));
}

static void LoadLibraryTreeUiState(MainWindow* win) {
    if (!win) {
        return;
    }
    Str data = file::ReadFile(LibraryTreeUiPathTemp());
    if (!data) {
        return;
    }
    win->expandedLibraryCollections.Reset();
    struct St {
        MainWindow* win = nullptr;
    } st{win};
    auto onVal = [](St* s, json::Value* v) {
        if (!s || !s->win || !v) {
            return;
        }
        if (json::PathMatch(v->path, StrL("/expandedCollections"), StrL("*"))) {
            if (v->type == json::Type::Number || v->type == json::Type::String) {
                i64 id = v->value ? ParseInt64(v->value) : 0;
                if (id > 0 && !s->win->expandedLibraryCollections.Contains(id)) {
                    s->win->expandedLibraryCollections.Append(id);
                }
            }
            return;
        }
        if (json::PathMatch(v->path, StrL("/lastBookId")) &&
            (v->type == json::Type::Number || v->type == json::Type::String)) {
            i64 id = v->value ? ParseInt64(v->value) : 0;
            if (id > 0 && s->win->activeLibraryBookId <= 0) {
                s->win->activeLibraryBookId = id;
            }
        }
    };
    json::Parse(data, MkFunc1<St, json::Value*>(onVal, &st));
    str::Free(data);
    if (len(win->expandedLibraryCollections) > 0 || win->activeLibraryBookId > 0) {
        win->libraryExpansionInitialized = true;
    }
}

static void RememberExpandedCollections(MainWindow* win) {
    if (!win || !win->libraryTreeView || !win->libraryTreeView->treeModel || win->libraryModelFiltered) return;
    win->expandedLibraryCollections.Reset();
    auto* model = (LibraryTreeModel*)win->libraryTreeView->treeModel;
    RememberExpandedCollectionsRec(win, model->root);
    win->libraryExpansionInitialized = true;
    SaveLibraryTreeUiState(win);
}

static void OnLibraryTreeExpansionChanged(MainWindow* win) {
    RememberExpandedCollections(win);
}

void LibrarySaveUiState(MainWindow* win) {
    SaveLibraryTreeUiState(win);
}

static bool LibraryPathsEqual(Str a, Str b) {
    if (!a || !b) {
        return false;
    }
    return str::EqI(path::NormalizeTemp(a), path::NormalizeTemp(b));
}

static LibraryTreeItem* FindBookByPath(LibraryTreeItem* item, Str path) {
    if (!item || !path) {
        return nullptr;
    }
    if (item->kind == LibraryTreeKind::Book && LibraryPathsEqual(item->path, path)) {
        return item;
    }
    for (LibraryTreeItem* child : item->children) {
        LibraryTreeItem* found = FindBookByPath(child, path);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

static LibraryTreeItem* FindBookById(LibraryTreeItem* item, i64 bookId) {
    if (!item || bookId <= 0) {
        return nullptr;
    }
    if (item->kind == LibraryTreeKind::Book && item->bookId == bookId) {
        return item;
    }
    for (LibraryTreeItem* child : item->children) {
        LibraryTreeItem* found = FindBookById(child, bookId);
        if (found) {
            return found;
        }
    }
    return nullptr;
}

static Str CurrentPdfPath(MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    if (!IsPdfTab(tab)) {
        return Str();
    }
    return tab->filePath;
}

// TreeView selection is the "currently viewed book" highlight. Rebuilds and
// tab switches must restore it from the active library book or current PDF.
void SyncLibrarySelection(MainWindow* win) {
    if (!win || !win->libraryTreeView || !win->libraryTreeView->treeModel) {
        return;
    }
    auto* model = (LibraryTreeModel*)win->libraryTreeView->treeModel;
    LibraryTreeItem* book = nullptr;
    if (win->activeLibraryBookId > 0) {
        book = FindBookById(model->root, win->activeLibraryBookId);
    }
    if (!book) {
        Str path = CurrentPdfPath(win);
        book = FindBookByPath(model->root, path);
    }
    TreeItem want = book ? (TreeItem)book : TreeModel::kNullItem;
    if (win->libraryTreeView->GetSelection() == want) {
        return;
    }
    logf("SyncLibrarySelection: bookId=%lld book=%d\n", win->activeLibraryBookId, book ? 1 : 0);
    win->libraryTreeView->SelectItem(want);
    if (!book) {
        return;
    }
    HTREEITEM hi = win->libraryTreeView->GetHandleByTreeItem((TreeItem)book);
    if (!hi) {
        return;
    }
    RECT itemRc{};
    RECT clientRc{};
    if (TreeView_GetItemRect(win->libraryTreeView->hwnd, hi, &itemRc, FALSE) &&
        GetClientRect(win->libraryTreeView->hwnd, &clientRc)) {
        if (itemRc.top >= 0 && itemRc.bottom <= clientRc.bottom) {
            return;
        }
    }
    TreeView_EnsureVisible(win->libraryTreeView->hwnd, hi);
}

static char gLastLibraryRefreshTiming[160];

void RefreshLibraryPanel(MainWindow* win) {
    if (!win || !win->libraryTreeView) return;
    logf("RefreshLibraryPanel: begin\n");
    if (win->libraryDragging) ReleaseCapture();
    win->libraryDragItem = 0;
    win->libraryDropItem = 0;
    win->libraryDragging = false;
    win->libraryDropAfter = false;
    win->libraryDropInto = false;
    win->libraryMultiSelected.Reset();
    win->librarySelectAnchor = 0;
    TimeStamp t0 = TimeGet();
    RememberExpandedCollections(win);
    double rememberMs = TimeSinceInMs(t0);
    TempStr filter = FilterTextTemp(win);
    TreeModel* previous = win->libraryTreeView->treeModel;
    t0 = TimeGet();
    LibraryTreeModel* model = BuildModel(win, filter);
    double buildMs = TimeSinceInMs(t0);
    t0 = TimeGet();
    win->libraryTreeView->SetTreeModel(model);
    double populateMs = TimeSinceInMs(t0);
    win->libraryModelFiltered = !!filter;
    win->libraryExpansionInitialized = true;
    t0 = TimeGet();
    delete previous;
    double freeMs = TimeSinceInMs(t0);
    t0 = TimeGet();
    SyncLibrarySelection(win);
    double syncMs = TimeSinceInMs(t0);
    snprintf(gLastLibraryRefreshTiming, sizeof(gLastLibraryRefreshTiming),
             "remember=%.1f build=%.1f populate=%.1f free=%.1f sync=%.1f", rememberMs, buildMs, populateMs, freeMs,
             syncMs);
    logf("RefreshLibraryPanel: end %s ms\n", Str(gLastLibraryRefreshTiming));
}

void RefreshLibraryPanels() {
    for (MainWindow* win : gWindows) {
        RefreshLibraryPanel(win);
    }
}

// Chrome-like: update a web book's library row label to the live tab title
// without rebuilding the whole tree (avoids freeze on rapid title changes).
void LibraryUpdateWebBookTabTitle(i64 bookId, Str title) {
    if (bookId <= 0 || !title || title.len == 0) {
        return;
    }
    if (str::StartsWithI(title, StrL("http://")) || str::StartsWithI(title, StrL("https://"))) {
        return;
    }
    if (LibraryIsAvailable()) {
        LibraryBook* book = LibraryStoreFindBookById(LibraryGetStore(), bookId);
        if (book && book->kind == LibraryBookKind::Web && (!book->title || !str::Eq(book->title, title))) {
            LibraryStoreSetBookTitle(LibraryGetStore(), bookId, title);
        }
        DeleteLibraryBook(book);
    }
    TempStr label = fmt("🌐 %s", title);
    for (MainWindow* win : gWindows) {
        if (!win || !win->libraryTreeView || !win->libraryTreeView->treeModel) {
            continue;
        }
        auto* model = (LibraryTreeModel*)win->libraryTreeView->treeModel;
        LibraryTreeItem* item = FindBookById(model->root, bookId);
        if (!item || item->bookKind != LibraryBookKind::Web) {
            continue;
        }
        if (str::Eq(item->text, label)) {
            continue;
        }
        str::ReplaceWithCopy(&item->text, label);
        win->libraryTreeView->UpdateItem((TreeItem)item);
    }
}

constexpr UINT_PTR kLibraryFilterTimerId = 0x4c46;
constexpr UINT kLibraryFilterDelayMs = 150;

static void CALLBACK OnLibraryFilterTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (win && win->libraryTreeView && win->libraryTreeView->hwnd == hwnd) {
        RefreshLibraryPanel(win);
    }
}

// Rebuilding a 10k-row tree per keystroke stalls typing; refresh once typing pauses.
static void OnFilterChanged(MainWindow* win) {
    if (!win || !win->libraryTreeView || !win->libraryTreeView->hwnd) {
        return;
    }
    SetTimer(win->libraryTreeView->hwnd, kLibraryFilterTimerId, kLibraryFilterDelayMs, OnLibraryFilterTimer);
}

static void OpenLibraryItem(MainWindow* source, LibraryTreeItem* item) {
    if (!item) {
        return;
    }
    if (item->bookKind == LibraryBookKind::Web) {
        OpenLibraryWebBook(source, item->bookId);
        return;
    }
    if (!item->path) {
        return;
    }
    MainWindow* existing = FindMainWindowByFile(item->path, true);
    if (existing) {
        existing->Focus();
        if (existing != source) {
            SyncLibrarySelection(source);
        }
        // Still notify so canvas XOR / AI bindings refresh for this PDF.
        LibraryOnActiveBookChanged(existing, item->bookId, (int)LibraryBookKind::Pdf);
        return;
    }
    LoadArgs args(item->path, source);
    StartLoadDocument(&args);
    LibraryOnActiveBookChanged(source, item->bookId, (int)LibraryBookKind::Pdf);
}

constexpr UINT_PTR kLibraryOpenTimerId = 0x4c4f;
static int gLibraryTimerOpens = 0;

static void CALLBACK OnLibraryOpenTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win || !win->libraryTreeView || win->libraryTreeView->hwnd != hwnd) {
        return;
    }
    i64 bookId = win->libraryPendingOpenBookId;
    win->libraryPendingOpenBookId = 0;
    gLibraryTimerOpens++;
    auto* model = (LibraryTreeModel*)win->libraryTreeView->treeModel;
    LibraryTreeItem* item = model && bookId > 0 ? FindBookById(model->root, bookId) : nullptr;
    OpenLibraryItem(win, item);
}

// A switch can take a few hundred ms (tab switch, WebView surfaces). Opening from
// WM_TIMER, which is only delivered once no input or paint is pending, lets queued
// clicks all update the selection first; then only the last clicked book opens.
static void ScheduleOpenLibraryItem(MainWindow* win, LibraryTreeItem* item) {
    if (!item || item->bookId <= 0 || !win->libraryTreeView->hwnd) {
        OpenLibraryItem(win, item);
        return;
    }
    win->libraryPendingOpenBookId = item->bookId;
    SetTimer(win->libraryTreeView->hwnd, kLibraryOpenTimerId, 0, OnLibraryOpenTimer);
}

static void OnTreeTooltip(TreeView::GetTooltipEvent* ev) {
    auto* item = (LibraryTreeItem*)ev->treeItem;
    if (!item) {
        return;
    }
    if (item->bookKind == LibraryBookKind::Web && item->url) {
        str::BufSet(ev->info->pszText, ev->info->cchTextMax, item->url);
        return;
    }
    if (item->path) {
        str::BufSet(ev->info->pszText, ev->info->cchTextMax, item->path);
    }
}

static void SplitBookLabel(LibraryTreeItem* item, Str& stem, Str& ext) {
    Str name = item && item->text ? item->text : Str();
    if (str::EndsWithI(name, StrL(".pdf"))) {
        stem = Str(name.s, len(name) - 4);
        ext = StrL(".pdf");
        return;
    }
    stem = name;
    ext = item && item->path && str::EndsWithI(item->path, StrL(".pdf")) ? StrL(".pdf") : Str();
}

static bool IsCtrlDown() {
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
}

// Runs for every truncated visible row on each paint, so it is one AlphaBlend of a
// cached 1px-high premultiplied ramp; a GDI+ Graphics per step made scrolling lag.
static void DrawLibraryFade(HDC hdc, Rect rc, Color bg) {
    int fadeDx = std::min(rc.dx, DpiScale(22));
    if (fadeDx <= 0 || rc.dy <= 0) {
        return;
    }
    static HDC memDc = nullptr;
    static HBITMAP bmp = nullptr;
    static u32* bits = nullptr;
    static int bmpDx = 0;
    static int rampDx = 0;
    static Color rampBg = 0;
    if (fadeDx > bmpDx) {
        if (!memDc) {
            memDc = CreateCompatibleDC(nullptr);
        }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = fadeDx;
        bmi.bmiHeader.biHeight = 1;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* p = nullptr;
        HBITMAP newBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &p, nullptr, 0);
        if (!newBmp || !memDc) {
            return;
        }
        SelectObject(memDc, newBmp);
        if (bmp) {
            DeleteObject(bmp);
        }
        bmp = newBmp;
        bits = (u32*)p;
        bmpDx = fadeDx;
        rampDx = 0;
    }
    if (rampDx != fadeDx || rampBg != bg) {
        u32 r = GetRValue(bg), g = GetGValue(bg), b = GetBValue(bg);
        for (int x = 0; x < fadeDx; x++) {
            u32 a = 255u * (u32)(x + 1) / (u32)fadeDx;
            bits[x] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
        }
        GdiFlush();
        rampDx = fadeDx;
        rampBg = bg;
    }
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(hdc, rc.x + rc.dx - fadeDx, rc.y, fadeDx, rc.dy, memDc, 0, 0, fadeDx, 1, bf);
}

static void DrawLibraryItem(TreeView::CustomDrawEvent* ev, MainWindow* win) {
    auto* item = (LibraryTreeItem*)ev->treeItem;
    if (!item) {
        return;
    }
    TreeView* tv = ev->treeView;
    Rect labelRect{};
    if (!tv->GetItemRect(ev->treeItem, true, labelRect)) {
        return;
    }
    Rect itemRect{};
    tv->GetItemRect(ev->treeItem, false, itemRect);

    NMTVCUSTOMDRAW* tvcd = ev->nm;
    HDC hdc = tvcd->nmcd.hdc;
    NMCUSTOMDRAW* cd = &tvcd->nmcd;
    bool isSelected = (cd->uItemState & CDIS_SELECTED) != 0;
    if (!isSelected) {
        HTREEITEM hSel = TreeView_GetSelection(tv->hwnd);
        HTREEITEM hItem = tv->GetHandleByTreeItem(ev->treeItem);
        isSelected = hSel && hItem && hSel == hItem;
    }
    if (!isSelected && win) {
        for (uintptr_t sel : win->libraryMultiSelected) {
            if (sel == (uintptr_t)item) {
                isSelected = true;
                break;
            }
        }
    }
    bool isDrop = win && win->libraryDropItem && win->libraryDropItem == (uintptr_t)item;
    auto* dragSrc = win && win->libraryDragItem ? (LibraryTreeItem*)win->libraryDragItem : nullptr;
    bool dropAsSibling =
        isDrop && dragSrc &&
        ((item->kind == LibraryTreeKind::Book && dragSrc->kind == LibraryTreeKind::Book) ||
         (item->kind == LibraryTreeKind::Collection && dragSrc->kind == LibraryTreeKind::Collection &&
          !win->libraryDropInto));
    bool hasFocus = isSelected && GetFocus() == tv->hwnd;
    Color bgCol, txtCol;
    ResolveTreeFilterItemColors(hdc, itemRect, tv->bgColor, tv->textColor, isSelected || (isDrop && !dropAsSibling),
                                hasFocus, &bgCol, &txtCol);
    // Song-inspired pale row tint when not selected/drop-highlighted.
    if (item->bgColor && !isSelected && !(isDrop && !dropAsSibling)) {
        u8 r = (u8)((item->bgColor >> 16) & 0xff);
        u8 g = (u8)((item->bgColor >> 8) & 0xff);
        u8 b = (u8)(item->bgColor & 0xff);
        bgCol = MkRgb(r, g, b);
    }

    RECT client{};
    GetClientRect(tv->hwnd, &client);
    RECT drawRc = ToRECT(labelRect);
    drawRc.top = itemRect.dy > 0 ? itemRect.y : drawRc.top;
    drawRc.bottom = itemRect.dy > 0 ? itemRect.y + itemRect.dy : drawRc.bottom;
    drawRc.right = client.right;
    if (drawRc.right <= drawRc.left) {
        return;
    }
    Rect drawRect = ToRect(drawRc);
    GfxHdc gfx(hdc);
    // Cover the default TreeView label completely. POSTPAINT used to fade over
    // it, so the tail of the filename stayed visible under the pinned ".pdf".
    gfx.FillRect(drawRect, bgCol);
    Rect textRect = drawRect;
    textRect.Inflate(-2, -1);

    if (item->kind == LibraryTreeKind::Book) {
        Str stem, ext;
        SplitBookLabel(item, stem, ext);
        Size extSize = ext ? gfx.MeasureText(ext, tv->GetFont()) : Size{};
        int extDx = ext ? extSize.dx : 0;
        Rect stemRect = textRect;
        stemRect.dx = std::max(0, textRect.dx - extDx);
        Size stemSize = stem ? gfx.MeasureText(stem, tv->GetFont()) : Size{};
        if (stem && stemRect.dx > 0) {
            int saved = SaveDC(hdc);
            IntersectClipRect(hdc, stemRect.x, stemRect.y, stemRect.x + stemRect.dx, stemRect.y + stemRect.dy);
            gfx.DrawText(stem, stemRect, gfxTextVCenter | gfxTextEllipsis, tv->GetFont(), txtCol);
            RestoreDC(hdc, saved);
        }
        if (stemSize.dx > stemRect.dx) {
            DrawLibraryFade(hdc, stemRect, bgCol);
        }
        if (ext) {
            Rect extRect = textRect;
            extRect.x = stemRect.x + stemRect.dx;
            extRect.dx = extDx;
            gfx.FillRect(extRect, bgCol);
            gfx.DrawText(ext, extRect, gfxTextVCenter | gfxTextEllipsis, tv->GetFont(), txtCol);
        }
    } else if (item->kind == LibraryTreeKind::Collection) {
        // Folder emoji distinguishes shelves/categories from book rows.
        TempStr labeled = fmt("📁 %s", item->text ? item->text : StrL(""));
        gfx.DrawText(labeled, textRect, gfxTextVCenter | gfxTextEllipsis, tv->GetFont(), txtCol);
    } else {
        gfx.DrawText(item->text, textRect, gfxTextVCenter | gfxTextEllipsis, tv->GetFont(), txtCol);
    }
    if (dropAsSibling) {
        int y = win->libraryDropAfter ? drawRect.y + drawRect.dy - 1 : drawRect.y;
        Rect line{drawRect.x, y, drawRect.dx, 2};
        gfx.FillRect(line, txtCol);
    } else if (isDrop && !isSelected) {
        gfx.DrawRect(drawRect, txtCol);
    }
}

static void OnLibraryCustomDraw(TreeView::CustomDrawEvent* ev) {
    ev->result = CDRF_DODEFAULT;
    NMCUSTOMDRAW* cd = &ev->nm->nmcd;
    if (cd->dwDrawStage == CDDS_PREPAINT) {
        ev->result = CDRF_NOTIFYITEMDRAW;
        return;
    }
    if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        // Hide the default label; we paint stem + pinned extension ourselves.
        ev->nm->clrText = ev->nm->clrTextBk;
        // Suppress the themed selection/hover box: it spans the full row, and the part left of
        // our painted label showed up as a bordered tab. DrawLibraryItem re-derives selection.
        cd->uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS | CDIS_HOT);
        ev->result = CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
        return;
    }
    if (cd->dwDrawStage == CDDS_ITEMPOSTPAINT) {
        DrawLibraryItem(ev, FindMainWindowByHwnd(ev->treeView->hwnd));
        ev->result = CDRF_DODEFAULT;
    }
}

static i64 CollectionIdForItem(LibraryTreeItem* item) {
    if (!item) {
        return 0;
    }
    if (item->kind == LibraryTreeKind::Collection) {
        return item->collectionId;
    }
    if (item->kind == LibraryTreeKind::Book && item->parent && item->parent->kind == LibraryTreeKind::Collection) {
        return item->parent->collectionId;
    }
    return 0;
}

static bool IsLibraryItemInside(LibraryTreeItem* item, LibraryTreeItem* ancestor) {
    for (LibraryTreeItem* it = item; it; it = it->parent) {
        if (it == ancestor) {
            return true;
        }
    }
    return false;
}

static void SetLibraryDropItem(MainWindow* win, TreeItem item, bool dropAfter, bool dropInto = false) {
    if (!win || (win->libraryDropItem == (uintptr_t)item && win->libraryDropAfter == dropAfter &&
                 win->libraryDropInto == dropInto)) {
        return;
    }
    win->libraryDropItem = (uintptr_t)item;
    win->libraryDropAfter = dropAfter;
    win->libraryDropInto = dropInto;
    if (win->libraryTreeView) {
        HwndInvalidate(win->libraryTreeView->hwnd);
    }
}

enum {
    kLibraryMenuOpenFolder = 1,
    kLibraryMenuCopyFilePath,
    kLibraryMenuCopyDirPath,
    kLibraryMenuRemoveBook,
    kLibraryMenuDeleteCollection,
    kLibraryMenuRenameCollection,
    kLibraryMenuNewCategory,
    kLibraryMenuAddNotebookLm,
    kLibraryMenuSelectNotebookLm,
    kLibraryMenuRenameBook,
    kLibraryMenuSetBookPath,
    kLibraryMenuViewDb,
    kLibraryMenuCompressPdf,
    kLibraryMenuScriptManager,
    kLibraryMenuViewAiTabs,
    kLibraryMenuCloseWebTab,
    kLibraryMenuNewTag,
    kLibraryMenuRetagRoot,
    kLibraryMenuColorNone,
    kLibraryMenuColorFirst,
};
// one id per folder in the "添加标签" submenu; above the color swatch ids
constexpr int kLibraryMenuTagFirst = 1000;

// Pale Song-dynasty inspired row tints (flat, low chroma). Values are 0x00RRGGBB.
struct LibraryBgSwatch {
    const char* name;
    u32 rgb;
};

static const LibraryBgSwatch gLibraryBgSwatches[] = {
    {"月白", 0xD6ECF0}, {"青白", 0xC0EBD7}, {"天水碧", 0xD5EBED}, {"艾绿", 0xD5EFDF},
    {"竹青", 0xE2EAD9}, {"水色", 0xDCE8E6}, {"藕荷", 0xF0E0E6}, {"藕色", 0xF5E6EA},
    {"缃色", 0xF4ECD4},
};

static HBITMAP CreateFlatColorSwatch(u32 rgb, int size) {
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = -size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        return nullptr;
    }
    u8 r = (u8)((rgb >> 16) & 0xff);
    u8 g = (u8)((rgb >> 8) & 0xff);
    u8 b = (u8)(rgb & 0xff);
    // Slightly deeper edge for a flat chip outline without shadows.
    u8 er = (u8)(r * 85 / 100);
    u8 eg = (u8)(g * 85 / 100);
    u8 eb = (u8)(b * 85 / 100);
    auto* px = (u32*)bits;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            bool edge = x == 0 || y == 0 || x == size - 1 || y == size - 1;
            u8 cr = edge ? er : r;
            u8 cg = edge ? eg : g;
            u8 cb = edge ? eb : b;
            // BGRA8 in memory: B | G<<8 | R<<16 | A<<24
            px[y * size + x] = cb | ((u32)cg << 8) | ((u32)cr << 16) | (0xffu << 24);
        }
    }
    return hbmp;
}

static void AppendLibraryColorMenu(HMENU parent, Vec<HBITMAP>& bitmaps) {
    HMENU colorMenu = CreatePopupMenu();
    AppendMenuW(colorMenu, MF_STRING, kLibraryMenuColorNone, CWStrTemp(_TRA("无")));
    AppendMenuW(colorMenu, MF_SEPARATOR, 0, nullptr);
    int swatchSize = DpiScale(22);
    for (int i = 0; i < dimof(gLibraryBgSwatches); i++) {
        HBITMAP hbmp = CreateFlatColorSwatch(gLibraryBgSwatches[i].rgb, swatchSize);
        if (hbmp) {
            bitmaps.Append(hbmp);
        }
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP | MIIM_FTYPE;
        mii.fType = MFT_STRING;
        mii.wID = kLibraryMenuColorFirst + i;
        mii.dwTypeData = (LPWSTR)CWStrTemp(gLibraryBgSwatches[i].name);
        mii.hbmpItem = hbmp;
        InsertMenuItemW(colorMenu, GetMenuItemCount(colorMenu), TRUE, &mii);
    }
    AppendMenuW(parent, MF_POPUP, (UINT_PTR)colorMenu, CWStrTemp(_TRA("背景颜色")));
}

static Str PromptLibraryText(HWND parent, Str title, Str label);
static bool CreateNamedCollection(MainWindow* win, i64 parentId, bool isShelf);

static bool CollectionExists(Vec<LibraryCollection*>& cols, i64 id) {
    for (LibraryCollection* c : cols) {
        if (c->id == id) {
            return true;
        }
    }
    return false;
}

// checkedIds: shown checked and inert; disabledIds: greyed out
static void AppendTagMenuLevel(HMENU menu, Vec<LibraryCollection*>& cols, i64 parentId, int depth,
                               Vec<i64>& checkedIds, Vec<i64>& menuTagIds, Vec<i64>* disabledIds = nullptr) {
    if (depth > 32) {
        return;
    }
    for (LibraryCollection* c : cols) {
        bool atLevel = depth == 0 ? (c->parentId == 0 || !CollectionExists(cols, c->parentId)) : c->parentId == parentId;
        if (!atLevel) {
            continue;
        }
        str::Builder label;
        for (int i = 0; i < depth; i++) {
            label.Append(StrL("\xe3\x80\x80\xe3\x80\x80")); // two ideographic spaces per level
        }
        label.Append(StrL("📁 "));
        Str name = c->name ? c->name : StrL("");
        for (int i = 0; i < len(name); i++) {
            // '&' would turn into a menu mnemonic
            if (name.s[i] == '&') {
                label.AppendChar('&');
            }
            label.AppendChar(name.s[i]);
        }
        UINT flags = MF_STRING;
        if (checkedIds.Contains(c->id)) {
            flags |= MF_CHECKED | MF_GRAYED;
        } else if (disabledIds && disabledIds->Contains(c->id)) {
            flags |= MF_GRAYED;
        }
        AppendMenuW(menu, flags, kLibraryMenuTagFirst + len(menuTagIds), CWStrTemp(ToStr(label)));
        menuTagIds.Append(c->id);
        AppendTagMenuLevel(menu, cols, c->id, depth + 1, checkedIds, menuTagIds, disabledIds);
    }
}

// Folder "修改标签": the folder (tag X) moves under the picked folder T, so its books become
// tagged "T/X". The folder itself
// is checked (picking it does nothing); its own sub-folders are greyed out (would be a cycle).
static void AppendCollectionRetagMenu(HMENU parent, i64 collectionId, Vec<i64>& menuTagIds) {
    HMENU tagMenu = CreatePopupMenu();
    Vec<LibraryCollection*> cols = LibraryStoreGetCollections(LibraryGetStore());
    Vec<i64> self;
    self.Append(collectionId);
    Vec<i64> descendants;
    Vec<i64> frontier;
    frontier.Append(collectionId);
    while (len(frontier) > 0) {
        i64 id = frontier.Pop();
        for (LibraryCollection* c : cols) {
            if (c->parentId == id && !descendants.Contains(c->id)) {
                descendants.Append(c->id);
                frontier.Append(c->id);
            }
        }
    }
    i64 parentId = 0;
    for (LibraryCollection* c : cols) {
        if (c->id == collectionId) {
            parentId = c->parentId;
        }
    }
    AppendMenuW(tagMenu, parentId == 0 ? MF_STRING | MF_CHECKED | MF_GRAYED : MF_STRING, kLibraryMenuRetagRoot,
                CWStrTemp(_TRA("图书馆（顶层）")));
    AppendMenuW(tagMenu, MF_SEPARATOR, 0, nullptr);
    AppendTagMenuLevel(tagMenu, cols, 0, 0, self, menuTagIds, &descendants);
    DeleteLibraryCollections(cols);
    AppendMenuW(parent, MF_POPUP, (UINT_PTR)tagMenu, CWStrTemp(_TRA("修改标签")));
}

// Folders are tags: menuTagIds[cmd - kLibraryMenuTagFirst] is the folder picked
static void AppendLibraryTagMenu(HMENU parent, i64 bookId, Vec<i64>& menuTagIds) {
    HMENU tagMenu = CreatePopupMenu();
    AppendMenuW(tagMenu, MF_STRING, kLibraryMenuNewTag, CWStrTemp(_TRA("新建标签...")));
    Vec<LibraryCollection*> cols = LibraryStoreGetCollections(LibraryGetStore());
    if (len(cols) > 0) {
        AppendMenuW(tagMenu, MF_SEPARATOR, 0, nullptr);
        Vec<i64> bookTags = LibraryStoreGetBookCollectionIds(LibraryGetStore(), bookId);
        AppendTagMenuLevel(tagMenu, cols, 0, 0, bookTags, menuTagIds);
    }
    DeleteLibraryCollections(cols);
    AppendMenuW(parent, MF_POPUP, (UINT_PTR)tagMenu, CWStrTemp(_TRA("添加标签")));
}

static bool AddLibraryBookTag(MainWindow* win, i64 bookId, i64 collectionId) {
    if (!LibraryStoreAddBookTag(LibraryGetStore(), bookId, collectionId)) {
        MessageBoxW(win->hwndFrame, CWStrTemp(fmt("%s\n%s", _TRA("添加标签失败。"), LibraryGetError())),
                    CWStrTemp(_TRA("添加标签")), MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

static LibraryTreeItem* FindCollectionItemRec(LibraryTreeItem* item, i64 collectionId) {
    if (item->kind == LibraryTreeKind::Collection && item->collectionId == collectionId) {
        return item;
    }
    for (LibraryTreeItem* child : item->children) {
        if (child->kind == LibraryTreeKind::Collection || child->kind == LibraryTreeKind::Root) {
            LibraryTreeItem* found = FindCollectionItemRec(child, collectionId);
            if (found) {
                return found;
            }
        }
    }
    return nullptr;
}

// after a refresh: expand down to the book inside the folder it was just tagged with
static void RevealBookInCollection(MainWindow* win, i64 collectionId, i64 bookId) {
    TreeView* tv = win->libraryTreeView;
    auto* model = tv ? (LibraryTreeModel*)tv->treeModel : nullptr;
    LibraryTreeItem* col = model ? FindCollectionItemRec(model->root, collectionId) : nullptr;
    if (!col) {
        return;
    }
    LibraryTreeItem* target = col;
    for (LibraryTreeItem* child : col->children) {
        if (child->kind == LibraryTreeKind::Book && child->bookId == bookId) {
            target = child;
            break;
        }
    }
    HTREEITEM hi = tv->EnsureHandleByTreeItem((TreeItem)target);
    if (hi) {
        TreeView_EnsureVisible(tv->hwnd, hi);
    }
}

static void UpdateOpenTabsAfterRename(Str oldPath, Str newPath) {
    if (!oldPath || !newPath) {
        return;
    }
    for (MainWindow* w : gWindows) {
        if (!w) {
            continue;
        }
        for (int i = 0; i < w->TabCount(); i++) {
            WindowTab* tab = w->GetTab(i);
            if (tab && tab->filePath && str::EqI(tab->filePath, oldPath)) {
                str::ReplaceWithCopy(&tab->filePath, newPath);
            }
        }
    }
}

static void ShowLibraryBookDbEntry(MainWindow* win, i64 bookId) {
    LibraryBook* book = LibraryStoreFindBookById(LibraryGetStore(), bookId);
    if (!book) {
        MessageBoxW(win ? win->hwndFrame : nullptr, CWStrTemp(_TRA("未找到该图书条目。")), CWStrTemp(_TRA("数据库条目")),
                    MB_OK | MB_ICONWARNING);
        return;
    }
    str::Builder out;
    out.Append(StrL("=== books ===\n"));
    out.Append(fmt("id:              %lld\n", book->id));
    out.Append(fmt("kind:            %s\n", book->kind == LibraryBookKind::Web ? StrL("web") : StrL("pdf")));
    out.Append(fmt("title:           %s\n", book->title ? book->title : StrL("")));
    out.Append(fmt("path:            %s\n", book->path ? book->path : StrL("")));
    out.Append(fmt("url:             %s\n", book->url ? book->url : StrL("")));
    out.Append(fmt("open_count:      %lld\n", book->openCount));
    out.Append(fmt("reading_seconds: %lld\n", book->readingSeconds));
    out.Append(fmt("last_read_ms:    %lld\n", book->lastReadMs));
    out.Append(fmt("bg_color:        0x%06X\n", book->bgColor));
    if (book->kind == LibraryBookKind::Pdf) {
        out.Append(fmt("file_exists:     %s\n", (book->path && file::Exists(book->path)) ? StrL("yes") : StrL("NO")));
    }
    out.Append(StrL("\n=== notebooklm JSON ===\n"));
    if (book->notebooklm && book->notebooklm.len > 0) {
        out.Append(book->notebooklm);
        out.AppendChar('\n');
    } else {
        out.Append(StrL("(empty)\n"));
    }

    out.Append(StrL("\n=== 归属 ===\n"));
    {
        Vec<LibraryBook*> manual =
            LibraryStoreGetBooks(LibraryGetStore(), LibraryBookScope::ManualRoot, 0, LibrarySort::Manual, Str());
        bool inManual = false;
        for (LibraryBook* b : manual) {
            if (b->id == bookId) {
                inManual = true;
                out.Append(fmt("manual_books:    yes (sort_pos=%lld)\n", b->sortPos));
                break;
            }
        }
        if (!inManual) {
            out.Append(StrL("manual_books:    no\n"));
        }
        DeleteLibraryBooks(manual);

        Vec<LibraryBook*> desk =
            LibraryStoreGetBooks(LibraryGetStore(), LibraryBookScope::Desk, 0, LibrarySort::Title, Str());
        bool onDesk = false;
        for (LibraryBook* b : desk) {
            if (b->id == bookId) {
                onDesk = true;
                break;
            }
        }
        out.Append(fmt("desk_books:      %s\n", onDesk ? StrL("yes") : StrL("no")));
        DeleteLibraryBooks(desk);

        Vec<LibraryCollection*> cols = LibraryStoreGetCollections(LibraryGetStore());
        bool anyCol = false;
        for (LibraryCollection* c : cols) {
            Vec<LibraryBook*> inCol =
                LibraryStoreGetBooks(LibraryGetStore(), LibraryBookScope::Collection, c->id, LibrarySort::Manual, Str());
            for (LibraryBook* b : inCol) {
                if (b->id == bookId) {
                    out.Append(fmt("collection:      id=%lld name=%s sort_pos=%lld\n", c->id,
                                   c->name ? c->name : StrL(""), b->sortPos));
                    anyCol = true;
                }
            }
            DeleteLibraryBooks(inCol);
        }
        if (!anyCol) {
            out.Append(StrL("collection:      (none)\n"));
        }
        DeleteLibraryCollections(cols);
    }

    out.Append(StrL("\n=== 调试提示 ===\n"));
    out.Append(StrL("数据库: %OneDrive%\\SumatraPDF\\SumatraPDF-library.db\n"));
    out.Append(StrL("切换 PDF 白屏时检查 WebPanel/jobs/pending 的 select-*.json 是否堆积\n"));

    ShowTextInWindowDialog(_TRA("图书馆数据库条目"), ToStr(out));
    DeleteLibraryBook(book);
}

static void CollectBooksInOrder(LibraryTreeItem* item, Vec<LibraryTreeItem*>* out) {
    if (!item || !out) {
        return;
    }
    if (item->kind == LibraryTreeKind::Book) {
        out->Append(item);
    }
    for (LibraryTreeItem* child : item->children) {
        CollectBooksInOrder(child, out);
    }
}

// Next book in tree order after bookId; if closing the last, return the previous.
static bool FindNeighborBookIds(LibraryTreeItem* root, i64 bookId, i64* nextIdOut, int* nextKindOut) {
    if (!root || bookId <= 0 || !nextIdOut || !nextKindOut) {
        return false;
    }
    *nextIdOut = 0;
    *nextKindOut = 0;
    Vec<LibraryTreeItem*> books;
    CollectBooksInOrder(root, &books);
    for (int i = 0; i < len(books); i++) {
        if (books[i]->bookId != bookId) {
            continue;
        }
        LibraryTreeItem* next = nullptr;
        if (i + 1 < len(books)) {
            next = books[i + 1];
        } else if (i > 0) {
            next = books[i - 1];
        }
        if (!next) {
            return false;
        }
        *nextIdOut = next->bookId;
        *nextKindOut = (int)next->bookKind;
        return true;
    }
    return false;
}

static void OnTreeContextMenu(ContextMenuEvent* ev) {
    MainWindow* win = FindMainWindowByHwnd(ev->w->hwnd);
    if (!win) return;
    TreeItem selected = win->libraryTreeView->GetItemAt(ev->mouseWindow.x, ev->mouseWindow.y);
    if (!selected) selected = win->libraryTreeView->GetSelection();
    auto* item = (LibraryTreeItem*)selected;
    if (!item) return;
    HMENU menu = CreatePopupMenu();
    Vec<HBITMAP> swatchBitmaps;
    Vec<i64> menuTagIds;
    if (item->kind == LibraryTreeKind::Book) {
        const bool isWeb = item->bookKind == LibraryBookKind::Web;
        if (!isWeb && item->path) {
            i64 bytes = file::GetSize(item->path);
            if (bytes >= 0) {
                // MB with 2 decimals (avoid depending on float fmt — compute manually).
                i64 hundredths = (bytes * 100 + 1024 * 1024 / 2) / (1024 * 1024);
                TempStr sizeLabel =
                    fmt(_TRA("大小: %lld.%02lld MB").s, hundredths / 100, hundredths % 100);
                AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, CWStrTemp(sizeLabel));
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            }
        }
        if (item->bookId > 0) {
            AppendLibraryTagMenu(menu, item->bookId, menuTagIds);
        }
        if (isWeb) {
            AppendMenuW(menu, MF_STRING, kLibraryMenuCopyFilePath, CWStrTemp(_TRA("复制 URL")));
            AppendMenuW(menu, MF_STRING, kLibraryMenuCloseWebTab, CWStrTemp(_TRA("关闭 Tab")));
        } else {
            AppendMenuW(menu, MF_STRING, kLibraryMenuOpenFolder, CWStrTemp(_TRA("在文件夹中显示")));
            AppendMenuW(menu, MF_STRING, kLibraryMenuCopyFilePath, CWStrTemp(_TRA("复制文件路径")));
            AppendMenuW(menu, MF_STRING, kLibraryMenuCopyDirPath, CWStrTemp(_TRA("复制文件所在目录路径")));
            AppendMenuW(menu, MF_STRING, kLibraryMenuCompressPdf, CWStrTemp(_TRA("压缩 PDF")));
        }
        AppendMenuW(menu, MF_STRING, kLibraryMenuScriptManager, CWStrTemp(_TRA("脚本管理")));
        if (!isWeb) {
            AppendMenuW(menu, MF_STRING, kLibraryMenuAddNotebookLm, CWStrTemp(_TRA("添加到 NotebookLM")));
            AppendMenuW(menu, MF_STRING, kLibraryMenuSelectNotebookLm, CWStrTemp(_TRA("在 NotebookLM 中仅选中此来源")));
            AppendMenuW(menu, MF_STRING, kLibraryMenuRenameBook, CWStrTemp(_TRA("重命名")));
            AppendMenuW(menu, MF_STRING, kLibraryMenuSetBookPath, CWStrTemp(_TRA("修改文件路径名...")));
        }
        AppendLibraryColorMenu(menu, swatchBitmaps);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kLibraryMenuViewAiTabs, CWStrTemp(_TRA("查看伴随 AI Tab 页")));
        AppendMenuW(menu, MF_STRING, kLibraryMenuViewDb, CWStrTemp(_TRA("查看数据库条目")));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kLibraryMenuRemoveBook, CWStrTemp(_TRA("从图书馆移除")));
    } else if (item->kind == LibraryTreeKind::Collection) {
        AppendMenuW(menu, MF_STRING, kLibraryMenuScriptManager, CWStrTemp(_TRA("脚本管理")));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kLibraryMenuNewCategory, CWStrTemp(_TRA("新建子文件夹")));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (item->collectionId > 0) {
            AppendCollectionRetagMenu(menu, item->collectionId, menuTagIds);
        }
        AppendLibraryColorMenu(menu, swatchBitmaps);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kLibraryMenuRenameCollection, CWStrTemp(_TRA("重命名文件夹")));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kLibraryMenuDeleteCollection, CWStrTemp(_TRA("删除文件夹")));
    }
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ev->mouseScreen.x, ev->mouseScreen.y, 0,
                             win->hwndFrame, nullptr);
    DestroyMenu(menu);
    for (HBITMAP hbmp : swatchBitmaps) {
        DeleteObject(hbmp);
    }
    bool changed = false;
    i64 revealTagId = 0;
    if (cmd == kLibraryMenuOpenFolder) {
        if (item->bookKind != LibraryBookKind::Web && item->path) {
            SumatraOpenPathInDefaultFileManager(item->path);
        }
    } else if (cmd == kLibraryMenuCopyFilePath) {
        if (item->bookKind == LibraryBookKind::Web) {
            if (item->url) {
                CopyTextToClipboard(item->url);
            }
        } else if (item->path) {
            CopyTextToClipboard(item->path);
        }
    } else if (cmd == kLibraryMenuCopyDirPath) {
        if (item->bookKind != LibraryBookKind::Web && item->path) {
            TempStr dir = path::GetDirTemp(item->path);
            if (dir) {
                CopyTextToClipboard(dir);
            }
        }
    } else if (cmd == kLibraryMenuCompressPdf) {
        if (item->bookKind != LibraryBookKind::Web && item->path) {
            ShowPdfCompressDialogForPath(win, item->path, false);
        }
    } else if (cmd == kLibraryMenuScriptManager) {
        ShowScriptManagerDialog(win);
    } else if (cmd == kLibraryMenuAddNotebookLm) {
        if (item->bookKind != LibraryBookKind::Web && item->path) {
            TempStr title = item->text ? item->text : item->path;
            TempStr uploadPath = item->path;
            i64 bytes = file::GetSize(item->path);
            if (bytes > kNotebookLmMaxUploadBytes) {
                TempStr compressed = CompressedPdfSiblingPathTemp(item->path);
                if (file::Exists(compressed)) {
                    uploadPath = compressed;
                    logf("LibraryAddNotebookLm: using compressed sibling '%s'\n", uploadPath);
                } else {
                    // Over NotebookLM 200 MB limit and no sibling yet — configure compress first.
                    MessageBoxW(win->hwndFrame,
                                CWStrTemp(_TRA("该 PDF 超过 NotebookLM 200 MB 上传限制。\n请先压缩，完成后将自动添加。")),
                                CWStrTemp(_TRA("压缩 PDF")), MB_OK | MB_ICONINFORMATION);
                    ShowPdfCompressDialogForPath(win, item->path, false, true, item->bookId, title);
                    return;
                }
            }
            WebPanelAddPdfToNotebookLm(win, item->bookId, uploadPath, title);
            uitask::Post(MkFunc0Void(WebPanelPollBridgeResults), "NotebookLmPoll");
        }
    } else if (cmd == kLibraryMenuSelectNotebookLm) {
        if (item->bookKind != LibraryBookKind::Web && item->path) {
            WebPanelSelectNotebookLmSource(win, item->bookId, item->path, item->text ? item->text : item->path);
        }
    } else if (cmd == kLibraryMenuRenameBook) {
        if (item->bookId > 0 && item->bookKind != LibraryBookKind::Web && item->path) {
            Str name = PromptLibraryText(win->hwndFrame, _TRA("重命名"), _TRA("新文件名（含扩展名）"));
            if (name) {
                TempStr base = name;
                if (!str::EndsWithI(base, StrL(".pdf"))) {
                    base = str::JoinTemp(base, StrL(".pdf"));
                }
                Str newPath = {};
                Str oldPath = str::Dup(item->path);
                if (LibraryStoreRenameBookFile(LibraryGetStore(), item->bookId, base, &newPath)) {
                    UpdateOpenTabsAfterRename(oldPath, newPath);
                    changed = true;
                } else {
                    MessageBoxW(win->hwndFrame, CWStrTemp(fmt("%s\n%s", _TRA("重命名失败。"), LibraryGetError())),
                                CWStrTemp(_TRA("重命名")), MB_OK | MB_ICONERROR);
                }
                str::Free(oldPath);
                str::Free(newPath);
                str::Free(name);
            }
        }
    } else if (cmd == kLibraryMenuSetBookPath) {
        if (item->bookKind != LibraryBookKind::Web && item->bookId > 0) {
            Str path = PromptLibraryText(win->hwndFrame, _TRA("修改文件路径名"),
                                         _TRA("新的完整路径（含文件名）"));
            if (path) {
                Str oldPath = str::Dup(item->path);
                if (LibraryStoreSetBookPath(LibraryGetStore(), item->bookId, path)) {
                    UpdateOpenTabsAfterRename(oldPath, path);
                    changed = true;
                } else {
                    MessageBoxW(win->hwndFrame,
                                CWStrTemp(fmt("%s\n%s", _TRA("修改路径失败。"), LibraryGetError())),
                                CWStrTemp(_TRA("修改文件路径名")), MB_OK | MB_ICONERROR);
                }
                str::Free(oldPath);
                str::Free(path);
            }
        }
    } else if (cmd == kLibraryMenuCloseWebTab) {
        // Close Tab = remove library web entry + browser tab, then open the next book.
        if (item->bookKind == LibraryBookKind::Web && item->bookId > 0) {
            i64 closedId = item->bookId;
            i64 nextId = 0;
            int nextKind = 0;
            auto* model = (LibraryTreeModel*)win->libraryTreeView->treeModel;
            bool hasNext = model && FindNeighborBookIds(model->root, closedId, &nextId, &nextKind);
            WebBrowserCloseTabForLibraryBook(win, closedId);
            WebPanelClearPdfTabIds(closedId);
            WebPanelClearPdfTabUrls(closedId);
            if (win->activeLibraryBookId == closedId) {
                win->activeLibraryBookId = 0;
            }
            if (LibraryStoreRemoveBook(LibraryGetStore(), closedId)) {
                RefreshLibraryPanels();
                if (hasNext && nextId > 0) {
                    if (nextKind == (int)LibraryBookKind::Web) {
                        OpenLibraryWebBook(win, nextId);
                    } else if (model) {
                        // Model was rebuilt; look up the PDF path from the new tree.
                        auto* neu = (LibraryTreeModel*)win->libraryTreeView->treeModel;
                        LibraryTreeItem* nextItem = neu ? FindBookById(neu->root, nextId) : nullptr;
                        if (nextItem) {
                            OpenLibraryItem(win, nextItem);
                        } else {
                            LibraryOnActiveBookChanged(win, nextId, nextKind);
                        }
                    }
                } else {
                    win->uiState.webBrowserVisible = false;
                    ApplyCenterContentSurface(win);
                    win->uiState.layout = {};
                    ScheduleUiUpdate(win, kUiRelayout | kUiNoToolbars);
                }
                LibrarySaveUiState(win);
            }
        }
    } else if (cmd == kLibraryMenuViewAiTabs) {
        if (item->bookId > 0) {
            WebPanelShowAiTabBindings(win, item->bookId);
        }
    } else if (cmd == kLibraryMenuViewDb) {
        ShowLibraryBookDbEntry(win, item->bookId);
    } else if (cmd == kLibraryMenuColorNone) {
        if (item->kind == LibraryTreeKind::Book) {
            changed = LibraryStoreSetBookBgColor(LibraryGetStore(), item->bookId, 0);
        } else if (item->kind == LibraryTreeKind::Collection) {
            changed = LibraryStoreSetCollectionBgColor(LibraryGetStore(), item->collectionId, 0);
        }
    } else if (cmd >= kLibraryMenuColorFirst && cmd < kLibraryMenuColorFirst + dimof(gLibraryBgSwatches)) {
        u32 rgb = gLibraryBgSwatches[cmd - kLibraryMenuColorFirst].rgb;
        if (item->kind == LibraryTreeKind::Book) {
            changed = LibraryStoreSetBookBgColor(LibraryGetStore(), item->bookId, rgb);
        } else if (item->kind == LibraryTreeKind::Collection) {
            changed = LibraryStoreSetCollectionBgColor(LibraryGetStore(), item->collectionId, rgb);
        }
    } else if (cmd == kLibraryMenuRemoveBook) {
        changed = LibraryStoreRemoveBook(LibraryGetStore(), item->bookId);
    } else if (cmd == kLibraryMenuDeleteCollection) {
        changed = LibraryStoreDeleteCollection(LibraryGetStore(), item->collectionId);
    } else if (cmd == kLibraryMenuRenameCollection) {
        Str name = PromptLibraryText(win->hwndFrame, _TRA("重命名文件夹"), _TRA("新名称"));
        if (name) {
            changed = LibraryStoreRenameCollection(LibraryGetStore(), item->collectionId, name);
            str::Free(name);
        }
    } else if (cmd == kLibraryMenuNewCategory) {
        changed = CreateNamedCollection(win, item->collectionId, false);
    } else if (cmd == kLibraryMenuNewTag) {
        Str name = PromptLibraryText(win->hwndFrame, _TRA("新建标签"), _TRA("标签名称（会创建同名文件夹）"));
        if (name && item->bookId > 0) {
            LibraryCollection* created = LibraryStoreCreateCollection(LibraryGetStore(), 0, false, name);
            if (created) {
                if (AddLibraryBookTag(win, item->bookId, created->id)) {
                    revealTagId = created->id;
                }
                DeleteLibraryCollection(created);
                changed = true;
            } else {
                MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("无法创建标签文件夹。")), CWStrTemp(_TRA("新建标签")),
                            MB_OK | MB_ICONWARNING);
            }
        }
        str::Free(name);
    } else if ((cmd >= kLibraryMenuTagFirst && cmd < kLibraryMenuTagFirst + len(menuTagIds)) ||
               cmd == kLibraryMenuRetagRoot) {
        i64 tagId = cmd == kLibraryMenuRetagRoot ? 0 : menuTagIds[cmd - kLibraryMenuTagFirst];
        if (item->kind == LibraryTreeKind::Collection) {
            if (tagId != item->collectionId) {
                changed = LibraryStoreRetagCollection(LibraryGetStore(), item->collectionId, tagId);
                if (changed) {
                    // gone if it merged into a same-named folder; the reveal is then a no-op
                    revealTagId = item->collectionId;
                } else {
                    MessageBoxW(win->hwndFrame, CWStrTemp(fmt("%s\n%s", _TRA("修改标签失败。"), LibraryGetError())),
                                CWStrTemp(_TRA("修改标签")), MB_OK | MB_ICONERROR);
                }
            }
        } else {
            changed = AddLibraryBookTag(win, item->bookId, tagId);
            if (changed) {
                revealTagId = tagId;
            }
        }
    }
    // item belongs to the model that the refresh deletes
    i64 revealBookId = item->bookId;
    if (changed) RefreshLibraryPanels();
    if (revealTagId > 0) {
        RevealBookInCollection(win, revealTagId, revealBookId);
    }
}

static HCURSOR gLibraryCursorMove = nullptr;
static HCURSOR gLibraryCursorCopy = nullptr;

// Build a 32x32 drag cursor: arrow tip + document rectangle; withPlus adds a
// small [+] badge at the rectangle's bottom-right (Ctrl = add tag / copy).
static HCURSOR CreateLibraryDragCursor(bool withPlus) {
    const int sz = 32;
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    auto* px = (u32*)bits;
    memset(px, 0, sz * sz * 4);
    auto put = [&](int x, int y, u32 argb) {
        if (x >= 0 && x < sz && y >= 0 && y < sz) {
            px[y * sz + x] = argb;
        }
    };
    // Simple black arrow (hotspot 0,0).
    for (int i = 0; i < 12; i++) {
        put(0, i, 0xFF000000);
        put(1, i, 0xFFFFFFFF);
    }
    for (int i = 0; i < 8; i++) {
        put(i, i, 0xFF000000);
        if (i + 1 < sz) put(i + 1, i, 0xFFFFFFFF);
    }
    put(0, 12, 0xFF000000);
    put(1, 11, 0xFF000000);
    put(2, 10, 0xFF000000);
    // Document rectangle under the arrow tip.
    const int rx = 8, ry = 10, rw = 14, rh = 16;
    for (int y = ry; y < ry + rh; y++) {
        for (int x = rx; x < rx + rw; x++) {
            bool edge = x == rx || x == rx + rw - 1 || y == ry || y == ry + rh - 1;
            put(x, y, edge ? 0xFF000000 : 0xFFFFFFFF);
        }
    }
    if (withPlus) {
        const int bx = rx + rw - 1, by = ry + rh - 1, bs = 9;
        for (int y = by; y < by + bs; y++) {
            for (int x = bx; x < bx + bs; x++) {
                bool edge = x == bx || x == bx + bs - 1 || y == by || y == by + bs - 1;
                put(x, y, edge ? 0xFF000000 : 0xFFFFFFFF);
            }
        }
        int cx = bx + bs / 2, cy = by + bs / 2;
        for (int d = -2; d <= 2; d++) {
            put(cx + d, cy, 0xFF000000);
            put(cx, cy + d, 0xFF000000);
        }
    }
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = FALSE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HCURSOR cur = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return cur;
}

static void EnsureLibraryDragCursors() {
    if (!gLibraryCursorMove) {
        gLibraryCursorMove = CreateLibraryDragCursor(false);
    }
    if (!gLibraryCursorCopy) {
        gLibraryCursorCopy = CreateLibraryDragCursor(true);
    }
}

static void ClearLibraryMultiSelection(MainWindow* win) {
    if (!win) {
        return;
    }
    win->libraryMultiSelected.Reset();
    win->librarySelectAnchor = 0;
}

static bool IsLibraryMultiSelected(MainWindow* win, LibraryTreeItem* item) {
    if (!win || !item) {
        return false;
    }
    uintptr_t key = (uintptr_t)item;
    for (uintptr_t sel : win->libraryMultiSelected) {
        if (sel == key) {
            return true;
        }
    }
    return false;
}

static void AddLibraryMultiSelected(MainWindow* win, LibraryTreeItem* item) {
    if (!win || !item || item->kind != LibraryTreeKind::Book) {
        return;
    }
    if (IsLibraryMultiSelected(win, item)) {
        return;
    }
    win->libraryMultiSelected.Append((uintptr_t)item);
}

static void RemoveLibraryMultiSelected(MainWindow* win, LibraryTreeItem* item) {
    if (!win || !item) {
        return;
    }
    uintptr_t key = (uintptr_t)item;
    for (int i = 0; i < len(win->libraryMultiSelected); i++) {
        if (win->libraryMultiSelected[i] == key) {
            win->libraryMultiSelected.RemoveAt(i);
            return;
        }
    }
}

static void SetLibraryMultiSelectionOne(MainWindow* win, LibraryTreeItem* item) {
    ClearLibraryMultiSelection(win);
    if (item && item->kind == LibraryTreeKind::Book) {
        win->libraryMultiSelected.Append((uintptr_t)item);
        win->librarySelectAnchor = (uintptr_t)item;
    }
}

static void CollectVisibleLibraryBooks(TreeView* tv, HTREEITEM hi, Vec<LibraryTreeItem*>* out) {
    if (!tv || !tv->hwnd || !out) {
        return;
    }
    for (; hi; hi = TreeView_GetNextSibling(tv->hwnd, hi)) {
        auto* item = (LibraryTreeItem*)tv->GetTreeItemByHandle(hi);
        if (item && item->kind == LibraryTreeKind::Book) {
            out->Append(item);
        }
        UINT state = TreeView_GetItemState(tv->hwnd, hi, TVIS_EXPANDED);
        if (state & TVIS_EXPANDED) {
            HTREEITEM child = TreeView_GetChild(tv->hwnd, hi);
            if (child) {
                CollectVisibleLibraryBooks(tv, child, out);
            }
        }
    }
}

static void SelectLibraryBookRange(MainWindow* win, LibraryTreeItem* from, LibraryTreeItem* to) {
    if (!win || !win->libraryTreeView || !from || !to) {
        return;
    }
    if (from->kind != LibraryTreeKind::Book || to->kind != LibraryTreeKind::Book) {
        SetLibraryMultiSelectionOne(win, to);
        return;
    }
    Vec<LibraryTreeItem*> visible;
    HTREEITEM root = TreeView_GetRoot(win->libraryTreeView->hwnd);
    CollectVisibleLibraryBooks(win->libraryTreeView, root, &visible);
    int i0 = -1, i1 = -1;
    for (int i = 0; i < len(visible); i++) {
        if (visible[i] == from) {
            i0 = i;
        }
        if (visible[i] == to) {
            i1 = i;
        }
    }
    if (i0 < 0 || i1 < 0) {
        SetLibraryMultiSelectionOne(win, to);
        return;
    }
    if (i0 > i1) {
        std::swap(i0, i1);
    }
    win->libraryMultiSelected.Reset();
    for (int i = i0; i <= i1; i++) {
        win->libraryMultiSelected.Append((uintptr_t)visible[i]);
    }
}

static void GetLibraryDragBooks(MainWindow* win, LibraryTreeItem* clicked, Vec<LibraryTreeItem*>* out) {
    if (!win || !out) {
        return;
    }
    out->Reset();
    if (len(win->libraryMultiSelected) > 0) {
        for (uintptr_t sel : win->libraryMultiSelected) {
            auto* item = (LibraryTreeItem*)sel;
            if (item && item->kind == LibraryTreeKind::Book) {
                out->Append(item);
            }
        }
        if (len(*out) > 0) {
            return;
        }
    }
    if (clicked && clicked->kind == LibraryTreeKind::Book) {
        out->Append(clicked);
    }
}

static void UpdateLibraryDragCursor() {
    EnsureLibraryDragCursors();
    bool copy = IsCtrlDown();
    HCURSOR cur = copy ? gLibraryCursorCopy : gLibraryCursorMove;
    if (!cur) {
        cur = LoadCursorW(nullptr, IDC_ARROW);
    }
    SetCursor(cur);
}

static void LibrarySelectTreeItemNoScrollJump(TreeView* tv, TreeItem ti) {
    if (!tv || !tv->hwnd || ti == TreeModel::kNullItem) {
        return;
    }
    HWND hwnd = tv->hwnd;
    HTREEITEM hi = tv->GetHandleByTreeItem(ti);
    HTREEITEM first = TreeView_GetFirstVisible(hwnd);
    RECT itemRc{};
    RECT clientRc{};
    bool fullyVisible = hi && TreeView_GetItemRect(hwnd, hi, &itemRc, FALSE) && GetClientRect(hwnd, &clientRc) &&
                        itemRc.top >= 0 && itemRc.bottom <= clientRc.bottom;
    tv->SelectItem(ti);
    // TreeView_SelectItem can scroll a mid-list folder to the top; pin scroll back.
    if (fullyVisible && first && TreeView_GetItemRect(hwnd, first, &itemRc, FALSE)) {
        HTREEITEM hSel = TreeView_GetSelection(hwnd);
        SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
        TreeView_SelectSetFirstVisible(hwnd, first);
        if (hSel) {
            TreeView_SelectItem(hwnd, hSel);
        }
        SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}

static LRESULT CALLBACK LibraryTreeSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    MainWindow* win = (MainWindow*)data;
    if (msg == WM_LBUTTONDOWN && win && win->libraryTreeView) {
        TVHITTESTINFO ht{};
        ht.pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        TreeView_HitTest(hwnd, &ht);
        if (ht.flags & TVHT_ONITEMBUTTON) {
            // Expand/collapse — leave to default TreeView handling.
            win->libraryDragItem = 0;
            win->libraryDragging = false;
            SetLibraryDropItem(win, 0, false);
            return DefSubclassProc(hwnd, msg, wp, lp);
        }
        auto* item = (LibraryTreeItem*)win->libraryTreeView->GetTreeItemByHandle(ht.hItem);
        win->libraryDragStart = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        win->libraryDragging = false;
        SetLibraryDropItem(win, 0, false);

        bool ctrl = (wp & MK_CONTROL) != 0;
        bool shift = (wp & MK_SHIFT) != 0;
        if (item && item->kind == LibraryTreeKind::Book) {
            if (shift && win->librarySelectAnchor) {
                auto* anchor = (LibraryTreeItem*)win->librarySelectAnchor;
                SelectLibraryBookRange(win, anchor, item);
            } else if (ctrl) {
                if (IsLibraryMultiSelected(win, item)) {
                    RemoveLibraryMultiSelected(win, item);
                } else {
                    AddLibraryMultiSelected(win, item);
                }
                win->librarySelectAnchor = (uintptr_t)item;
            } else {
                // Plain click: keep multi-select if clicking an already-selected book (for drag).
                if (!IsLibraryMultiSelected(win, item) || len(win->libraryMultiSelected) <= 1) {
                    SetLibraryMultiSelectionOne(win, item);
                }
                win->librarySelectAnchor = (uintptr_t)item;
            }
            win->libraryDragItem = (uintptr_t)item;
            LibrarySelectTreeItemNoScrollJump(win->libraryTreeView, (TreeItem)item);
            HwndInvalidate(hwnd);
            SetFocus(hwnd);
            return 0;
        }
        // Folder / empty: single selection, clear multi.
        ClearLibraryMultiSelection(win);
        win->libraryDragItem = item ? (uintptr_t)item : 0;
        if (item) {
            LibrarySelectTreeItemNoScrollJump(win->libraryTreeView, (TreeItem)item);
        }
        HwndInvalidate(hwnd);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_SETCURSOR && win && win->libraryDragging) {
        UpdateLibraryDragCursor();
        return TRUE;
    }
    if (msg == WM_KEYDOWN || msg == WM_KEYUP) {
        if (win && win->libraryDragging && (wp == VK_CONTROL)) {
            UpdateLibraryDragCursor();
        }
    }
    if (msg == WM_MOUSEMOVE && win && win->libraryDragItem && (wp & MK_LBUTTON)) {
        int dx = std::abs(GET_X_LPARAM(lp) - win->libraryDragStart.x);
        int dy = std::abs(GET_Y_LPARAM(lp) - win->libraryDragStart.y);
        if (!win->libraryDragging && (dx >= 3 || dy >= 3)) {
            win->libraryDragging = true;
            SetCapture(hwnd);
            UpdateLibraryDragCursor();
        }
        if (win->libraryDragging) {
            UpdateLibraryDragCursor();
            RECT clientRc{};
            GetClientRect(hwnd, &clientRc);
            int y = GET_Y_LPARAM(lp);
            int edge = DpiScale(28);
            if (y < edge) {
                SendMessageW(hwnd, WM_VSCROLL, SB_LINEUP, 0);
            } else if (y > clientRc.bottom - edge) {
                SendMessageW(hwnd, WM_VSCROLL, SB_LINEDOWN, 0);
            }
            auto* hover = (LibraryTreeItem*)win->libraryTreeView->GetItemAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            auto* dragSrc = (LibraryTreeItem*)win->libraryDragItem;
            bool draggingFolder = dragSrc && dragSrc->kind == LibraryTreeKind::Collection;
            bool folderOnFolder = draggingFolder && hover && hover->kind == LibraryTreeKind::Collection;
            if (folderOnFolder && IsLibraryItemInside(hover, dragSrc)) {
                hover = nullptr; // a folder cannot go into itself or its own sub-folders
                folderOnFolder = false;
            }
            Rect itemRc{};
            bool haveRc = hover && win->libraryTreeView->GetItemRect((TreeItem)hover, false, itemRc) && itemRc.dy > 0;
            int mouseY = GET_Y_LPARAM(lp);
            bool dropAfter = false;
            bool dropInto = false;
            if (folderOnFolder) {
                // Explorer-style zones: top/bottom quarter = reorder beside it, middle = into it.
                int quarter = haveRc ? itemRc.dy / 4 : 0;
                if (IsCtrlDown() || !haveRc) {
                    dropInto = true;
                } else if (mouseY < itemRc.y + quarter) {
                    dropAfter = false;
                } else if (mouseY >= itemRc.y + itemRc.dy - quarter) {
                    dropAfter = true;
                } else {
                    dropInto = true;
                }
            } else if (hover && hover->kind == LibraryTreeKind::Book && haveRc) {
                dropAfter = mouseY >= itemRc.y + itemRc.dy / 2;
            }
            // Auto-expand a folder when something is about to be dropped into it.
            if (hover && hover->kind == LibraryTreeKind::Collection &&
                (dropInto || (dragSrc && dragSrc->kind == LibraryTreeKind::Book))) {
                HTREEITEM hi = win->libraryTreeView->GetHandleByTreeItem((TreeItem)hover);
                if (hi) {
                    TreeView_Expand(hwnd, hi, TVE_EXPAND);
                }
            }
            SetLibraryDropItem(win, (TreeItem)hover, dropAfter, dropInto);
            return 0;
        }
    }
    if (msg == WM_MOUSEMOVE && win && !win->libraryDragging) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    }
    if (msg == WM_MOUSELEAVE) {
        HwndInvalidate(hwnd);
    }
    if (msg == WM_LBUTTONUP && win && win->libraryTreeView) {
        auto* source = (LibraryTreeItem*)win->libraryDragItem;
        auto* target = (LibraryTreeItem*)win->libraryDropItem;
        bool dropAfter = win->libraryDropAfter;
        bool dropInto = win->libraryDropInto;
        // while dragging, no drop item means the hovered row refused the drop
        if (!target && !win->libraryDragging) {
            target = (LibraryTreeItem*)win->libraryTreeView->GetItemAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        }
        bool wasDragging = win->libraryDragging;
        bool filtered = win->libraryModelFiltered;
        bool copy = IsCtrlDown(); // Ctrl = add tag (keep source membership)
        bool changed = false;
        i64 revealCollectionId = 0;
        if (wasDragging && source && source != target && !filtered) {
            Vec<LibraryTreeItem*> books;
            GetLibraryDragBooks(win, source, &books);
            if (len(books) > 0 &&
                (source->kind == LibraryTreeKind::Book || len(win->libraryMultiSelected) > 0)) {
                // Drop one or more books onto a folder or beside another book.
                for (LibraryTreeItem* book : books) {
                    if (!book || book == target) {
                        continue;
                    }
                    if (target && target->kind == LibraryTreeKind::Book) {
                        i64 srcCol = CollectionIdForItem(book);
                        i64 dstCol = CollectionIdForItem(target);
                        if (srcCol == dstCol) {
                            changed = LibraryStoreReorderBook(LibraryGetStore(), book->bookId, srcCol, target->bookId,
                                                             dropAfter) ||
                                      changed;
                        } else {
                            bool ok = LibraryStorePlaceBook(LibraryGetStore(), book->bookId, srcCol, dstCol, copy);
                            if (ok) {
                                changed = true;
                                LibraryStoreReorderBook(LibraryGetStore(), book->bookId, dstCol, target->bookId,
                                                        dropAfter);
                            }
                        }
                    } else {
                        changed = LibraryStorePlaceBook(LibraryGetStore(), book->bookId, CollectionIdForItem(book),
                                                        CollectionIdForItem(target), copy) ||
                                  changed;
                    }
                }
            } else if (source->kind == LibraryTreeKind::Collection) {
                if (target && target->kind == LibraryTreeKind::Collection) {
                    if (dropInto || copy) {
                        // Same as "修改标签": tag X becomes target/X (merges into a same-named folder).
                        if (target->collectionId != source->collectionId) {
                            changed = LibraryStoreRetagCollection(LibraryGetStore(), source->collectionId,
                                                                  target->collectionId);
                            if (changed) {
                                revealCollectionId = source->collectionId;
                            }
                        }
                    } else {
                        // Default: sibling reorder (before / after by drop half).
                        i64 destParent = 0;
                        if (target->parent && target->parent->kind == LibraryTreeKind::Collection) {
                            destParent = target->parent->collectionId;
                        }
                        i64 srcParent = 0;
                        if (source->parent && source->parent->kind == LibraryTreeKind::Collection) {
                            srcParent = source->parent->collectionId;
                        }
                        if (srcParent != destParent) {
                            changed = LibraryStoreMoveCollection(LibraryGetStore(), source->collectionId, destParent);
                        }                        changed = LibraryStoreReorderCollection(LibraryGetStore(), source->collectionId, destParent,
                                                               target->collectionId, dropAfter) ||
                                  changed;
                    }
                } else if (target && target->kind == LibraryTreeKind::Book) {
                    i64 newParent = CollectionIdForItem(target);
                    if (newParent != source->collectionId) {
                        changed = LibraryStoreMoveCollection(LibraryGetStore(), source->collectionId, newParent);
                    }
                }
            }
        }
        if (win->libraryDragging) {
            ReleaseCapture();
        }
        win->libraryDragItem = 0;
        win->libraryDragging = false;
        SetLibraryDropItem(win, 0, false);
        if (changed) {
            RefreshLibraryPanels();
            if (revealCollectionId > 0) {
                RevealBookInCollection(win, revealCollectionId, 0);
            }
            return 0;
        }
        // Open only on a simple single-book click (not a multi-select gesture).
        if (!wasDragging && source && source->kind == LibraryTreeKind::Book && len(win->libraryMultiSelected) <= 1) {
            ScheduleOpenLibraryItem(win, source);
            return 0;
        }
        return 0;
    }
    if (msg == WM_CAPTURECHANGED && win) {
        win->libraryDragItem = 0;
        win->libraryDragging = false;
        SetLibraryDropItem(win, 0, false);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void CloseLibrary(MainWindow* win, VirtMouseEvent*) {
    SetLibraryPanelVisible(win, false);
}

enum {
    kLibraryAddShelf = 1,
    kLibraryAddFolder,
    kLibraryAddPdf,
    kLibraryAddWeb,
    kLibraryAddWebClipboard,
    kLibraryImportDir,
    kLibraryReplacePath,
};

enum {
    kLibraryPromptEdit = 1001
};

struct LibraryPromptState {
    Str title;
    Str label;
    HWND edit = nullptr;
    Str result;
};

#pragma pack(push, 2)
struct LibraryPromptTemplate {
    DLGTEMPLATE dlg{};
    WORD menu = 0;
    WORD windowClass = 0;
    WCHAR title = 0;
};
#pragma pack(pop)

static INT_PTR CALLBACK LibraryPromptProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = (LibraryPromptState*)GetWindowLongPtrW(hwnd, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        state = (LibraryPromptState*)lp;
        SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)state);
        SetWindowTextW(hwnd, CWStrTemp(state->title));
        HFONT font = GetAppFont()->GetHFont();
        Rect rc = HwndClientRect(hwnd);
        HWND label = CreateWindowW(WC_STATICW, CWStrTemp(state->label), WS_CHILD | WS_VISIBLE, 12, 12, rc.dx - 24, 20,
                                   hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        state->edit =
            CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12,
                            35, rc.dx - 24, 24, hwnd, (HMENU)kLibraryPromptEdit, GetModuleHandleW(nullptr), nullptr);
        HWND ok =
            CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("OK")), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                          rc.dx - 174, rc.dy - 38, 76, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
        HWND cancel =
            CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("Cancel")), WS_CHILD | WS_VISIBLE | WS_TABSTOP, rc.dx - 88,
                          rc.dy - 38, 76, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
        HWND controls[] = {label, state->edit, ok, cancel};
        for (HWND control : controls) {
            SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
        }
        HwndSetFocus(state->edit);
        return FALSE;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK && state) {
        int n = GetWindowTextLengthW(state->edit);
        WCHAR* value = AllocArrayTemp<WCHAR>(n + 1);
        GetWindowTextW(state->edit, value, n + 1);
        state->result = str::Dup(ToUtf8Temp(value));
        str::TrimWSInPlace(state->result, str::TrimOpt::Both);
        if (!state->result) {
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
}

static Str PromptLibraryText(HWND parent, Str title, Str label) {
    LibraryPromptState state{title, label};
    LibraryPromptTemplate t;
    t.dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    t.dlg.dwExtendedStyle = WS_EX_DLGMODALFRAME;
    t.dlg.cx = 280;
    t.dlg.cy = 92;
    INT_PTR result =
        DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &t.dlg, parent, LibraryPromptProc, (LPARAM)&state);
    if (result != IDOK) {
        str::Free(state.result);
        return {};
    }
    return state.result;
}

static bool CreateNamedCollection(MainWindow* win, i64 parentId, bool isShelf) {
    Str title = isShelf ? _TRA("新建书架") : _TRA("新建文件夹");
    Str label = isShelf ? _TRA("书架名称") : _TRA("文件夹名称（标签）");
    Str name = PromptLibraryText(win->hwndFrame, title, label);
    if (!name) {
        return false;
    }
    LibraryCollection* created = LibraryStoreCreateCollection(LibraryGetStore(), parentId, isShelf, name);
    if (!created) {
        logf("Library failed to create %s '%s': %s\n", isShelf ? StrL("shelf") : StrL("folder"), name,
             LibraryGetError());
        MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("无法创建文件夹。")), CWStrTemp(title), MB_OK | MB_ICONWARNING);
        str::Free(name);
        return false;
    }
    logf("Library created %s '%s' id=%lld\n", isShelf ? StrL("shelf") : StrL("folder"), name, created->id);
    DeleteLibraryCollection(created);
    str::Free(name);
    return true;
}

static i64 SelectedCollectionId(MainWindow* win) {
    TreeItem selected = win->libraryTreeView ? win->libraryTreeView->GetSelection() : 0;
    auto* item = (LibraryTreeItem*)selected;
    if (item && item->kind == LibraryTreeKind::Collection) return item->collectionId;
    if (item && item->kind == LibraryTreeKind::Book && item->parent &&
        item->parent->kind == LibraryTreeKind::Collection) {
        return item->parent->collectionId;
    }
    return 0;
}

static bool AddPdfToLibrary(MainWindow* win, Str filePath) {
    LibraryBook* book =
        LibraryStoreAddBook(LibraryGetStore(), filePath, path::GetBaseNameTemp(filePath), UnixTimeMsNow());
    bool ok = book != nullptr;
    i64 collectionId = SelectedCollectionId(win);
    if (book && collectionId) {
        ok = LibraryStorePlaceBook(LibraryGetStore(), book->id, 0, collectionId, false);
    }
    DeleteLibraryBook(book);
    return ok;
}

static void AddPdfFiles(MainWindow* win) {
    constexpr DWORD cch = 64 * 1024;
    WCHAR* files = AllocArrayTemp<WCHAR>(cch);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = win->hwndFrame;
    WCHAR filter[128]{};
    WCHAR* filterName = CWStrTemp(_TRA("PDF files"));
    int filterNameLen = _snwprintf_s(filter, dimof(filter), _TRUNCATE, L"%s (*.pdf)", filterName);
    if (filterNameLen < 0) return;
    wcscpy_s(filter + filterNameLen + 1, dimof(filter) - filterNameLen - 1, L"*.pdf");
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = files;
    ofn.nMaxFile = cch;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&ofn)) return;

    WCHAR* first = files;
    WCHAR* next = first + wcslen(first) + 1;
    int imported = 0;
    if (!*next) {
        TempStr path = ToUtf8Temp(first);
        imported = AddPdfToLibrary(win, path) ? 1 : 0;
    } else {
        TempStr dir = ToUtf8Temp(first);
        while (*next) {
            TempStr path = path::JoinTemp(dir, ToUtf8Temp(next));
            if (AddPdfToLibrary(win, path)) imported++;
            next += wcslen(next) + 1;
        }
    }
    logf("Library file import: %d PDFs\n", imported);
    RefreshLibraryPanels();
}

static bool IsLibraryPathOpen(Str path) {
    for (MainWindow* win : gWindows) {
        for (WindowTab* tab : win->Tabs()) {
            if (IsPdfTab(tab) && str::EqI(path::NormalizeTemp(tab->filePath), path::NormalizeTemp(path))) return true;
        }
    }
    return false;
}

static void ReplaceLibraryPathPrefix(MainWindow* win) {
    Str oldPrefix = PromptLibraryText(win->hwndFrame, _TRA("Replace Library Paths"), _TRA("Old directory prefix"));
    if (!oldPrefix) return;
    Str newPrefix = PromptLibraryText(win->hwndFrame, _TRA("Replace Library Paths"), _TRA("New directory prefix"));
    if (!newPrefix) {
        str::Free(oldPrefix);
        return;
    }
    Vec<LibraryPathChange*> changes = LibraryStorePreviewPathReplace(LibraryGetStore(), oldPrefix, newPrefix);
    str::Builder preview;
    int missing = 0;
    int conflicts = 0;
    int opened = 0;
    for (LibraryPathChange* change : changes) {
        bool isOpen = IsLibraryPathOpen(change->oldPath);
        if (!change->targetExists) missing++;
        if (change->conflict) conflicts++;
        if (isOpen) opened++;
        preview.Append(fmt("%s\n  -> %s%s%s%s\n\n", change->oldPath, change->newPath,
                           change->targetExists ? Str() : _TRA("  [target missing]"),
                           change->conflict ? _TRA("  [path conflict]") : Str(),
                           isOpen ? _TRA("  [currently open]") : Str()));
    }
    preview.Append(fmt(_TRA("Books: %d, missing targets: %d, conflicts: %d, currently open: %d\n").s, len(changes),
                       missing, conflicts, opened));
    ShowTextInWindowDialog(_TRA("Library Path Replacement Preview"), ToStr(preview));
    bool canApply = len(changes) > 0 && conflicts == 0 && opened == 0;
    if (!canApply) {
        MessageBoxW(
            win->hwndFrame,
            CWStrTemp(_TRA("No paths can be changed, or the preview contains a conflict/currently open document.")),
            CWStrTemp(_TRA("Library Paths")), MB_OK | MB_ICONWARNING);
    } else if (MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("Apply all previewed path changes in one transaction?")),
                           CWStrTemp(_TRA("Library Paths")), MB_YESNO | MB_ICONQUESTION) == IDYES) {
        if (!LibraryStoreApplyPathReplace(LibraryGetStore(), changes)) {
            MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("The path replacement failed. No paths were changed.")),
                        CWStrTemp(_TRA("Library Paths")), MB_OK | MB_ICONERROR);
        } else {
            logf("Library path replacement: %d paths, %d missing targets\n", len(changes), missing);
            RefreshLibraryPanels();
        }
    }
    DeleteLibraryPathChanges(changes);
    str::Free(oldPrefix);
    str::Free(newPrefix);
}

static void ImportPdfDirectory(MainWindow* win) {
    BROWSEINFOW bi{};
    bi.hwndOwner = win->hwndFrame;
    bi.lpszTitle = CWStrTemp(_TRA("Import PDF directory"));
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    WCHAR pathW[MAX_PATH]{};
    bool ok = SHGetPathFromIDListW(pidl, pathW);
    CoTaskMemFree(pidl);
    if (!ok) return;
    TempStr dir = ToUtf8Temp(pathW);
    DirIter iter(dir);
    iter.recurse = true;
    int imported = 0;
    for (DirIterEntry* entry : iter) {
        if (!entry->isFile || !str::EndsWithI(entry->filePath, StrL(".pdf"))) continue;
        if (AddPdfToLibrary(win, entry->filePath)) imported++;
    }
    logf("Library directory import: '%s', %d PDFs\n", dir, imported);
    RefreshLibraryPanels();
}

static Str PromptLibraryMultiline(HWND parent, Str title, Str label) {
    LibraryPromptState state{title, label};
    LibraryPromptTemplate t;
    t.dlg.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    t.dlg.dwExtendedStyle = WS_EX_DLGMODALFRAME;
    t.dlg.cx = 360;
    t.dlg.cy = 180;
    auto proc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> INT_PTR {
        auto* state = (LibraryPromptState*)GetWindowLongPtrW(hwnd, DWLP_USER);
        if (msg == WM_INITDIALOG) {
            state = (LibraryPromptState*)lp;
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)state);
            SetWindowTextW(hwnd, CWStrTemp(state->title));
            HFONT font = GetAppFont()->GetHFont();
            Rect rc = HwndClientRect(hwnd);
            HWND labelHwnd =
                CreateWindowW(WC_STATICW, CWStrTemp(state->label), WS_CHILD | WS_VISIBLE, 12, 10, rc.dx - 24, 36, hwnd,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
            state->edit = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_EDITW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL, 12, 48,
                rc.dx - 24, rc.dy - 96, hwnd, (HMENU)kLibraryPromptEdit, GetModuleHandleW(nullptr), nullptr);
            HWND ok =
                CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("OK")), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              rc.dx - 174, rc.dy - 38, 76, 26, hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
            HWND cancel =
                CreateWindowW(WC_BUTTONW, CWStrTemp(_TRA("Cancel")), WS_CHILD | WS_VISIBLE | WS_TABSTOP, rc.dx - 88,
                              rc.dy - 38, 76, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
            for (HWND control : {labelHwnd, state->edit, ok, cancel}) {
                SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
            }
            HwndSetFocus(state->edit);
            return FALSE;
        }
        if (msg == WM_COMMAND && LOWORD(wp) == IDOK && state) {
            int n = GetWindowTextLengthW(state->edit);
            WCHAR* value = AllocArrayTemp<WCHAR>(n + 1);
            GetWindowTextW(state->edit, value, n + 1);
            state->result = str::Dup(ToUtf8Temp(value));
            str::TrimWSInPlace(state->result, str::TrimOpt::Both);
            if (!state->result) {
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

static TempStr NormalizeSiteUrlTemp(Str raw) {
    if (!raw) {
        return {};
    }
    TempStr s = str::DupTemp(raw);
    while (s.len > 0 && (s.s[0] == ' ' || s.s[0] == '\t' || s.s[0] == '"' || s.s[0] == '\'' || s.s[0] == '<' ||
                         s.s[0] == '(')) {
        s = Str(s.s + 1, s.len - 1);
    }
    while (s.len > 0) {
        char c = s.s[s.len - 1];
        if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '>' || c == ')' || c == ',' || c == ';' || c == '.') {
            s = Str(s.s, s.len - 1);
            continue;
        }
        break;
    }
    if (!s) {
        return {};
    }
    if (str::StartsWithI(s, StrL("http://")) || str::StartsWithI(s, StrL("https://"))) {
        return str::DupTemp(s);
    }
    if (str::StartsWithI(s, StrL("www."))) {
        return str::JoinTemp(StrL("https://"), s);
    }
    // Bare domains like example.com/path
    if (str::IndexOfChar(s, '.') >= 0 && str::IndexOfChar(s, ' ') < 0 && str::IndexOfChar(s, '\\') < 0) {
        return str::JoinTemp(StrL("https://"), s);
    }
    return {};
}

static void ParseSiteUrls(Str text, Vec<Str>* out) {
    if (!text || !out) {
        return;
    }
    int i = 0;
    while (i < text.len) {
        while (i < text.len && (text.s[i] == ' ' || text.s[i] == '\t' || text.s[i] == '\r' || text.s[i] == '\n')) {
            i++;
        }
        if (i >= text.len) {
            break;
        }
        int start = i;
        while (i < text.len && text.s[i] != ' ' && text.s[i] != '\t' && text.s[i] != '\r' && text.s[i] != '\n') {
            i++;
        }
        TempStr token = NormalizeSiteUrlTemp(Str(text.s + start, i - start));
        if (token) {
            out->Append(str::Dup(token));
        }
    }
}

static TempStr LibraryClipboardTextTemp() {
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

static i64 EnsureWebSitesCollectionId() {
    Vec<LibraryCollection*> cols = LibraryStoreGetCollections(LibraryGetStore());
    i64 id = 0;
    i64 legacyId = 0;
    for (LibraryCollection* c : cols) {
        if (c->parentId != 0 || !c->name) {
            continue;
        }
        if (str::Eq(c->name, StrL("网页"))) {
            id = c->id;
            break;
        }
        if (str::Eq(c->name, StrL("网站"))) {
            legacyId = c->id;
        }
    }
    DeleteLibraryCollections(cols);
    if (id > 0) {
        return id;
    }
    if (legacyId > 0) {
        LibraryStoreRenameCollection(LibraryGetStore(), legacyId, StrL("网页"));
        return legacyId;
    }
    LibraryCollection* created = LibraryStoreCreateCollection(LibraryGetStore(), 0, false, StrL("网页"));
    if (!created) {
        return 0;
    }
    id = created->id;
    DeleteLibraryCollection(created);
    return id;
}

// Add each URL as its own library web book (duplicates allowed) under 网页,
// and open a dedicated browser tab for each.
static bool IsLibraryEligibleHttpUrl(Str url) {
    if (!url) {
        return false;
    }
    // Only user-facing http(s) sites belong in the library — never extension
    // pages, edge internals, or blank popups (extension storms used to flood 网页).
    if (!(str::StartsWithI(url, StrL("http://")) || str::StartsWithI(url, StrL("https://")))) {
        return false;
    }
    if (str::StartsWithI(url, StrL("https://chrome.google.com/webstore")) ||
        str::StartsWithI(url, StrL("https://microsoftedge.microsoft.com/addons"))) {
        return false;
    }
    return true;
}

static void AddSitesToLibraryAndTabs(MainWindow* win, Vec<Str>& urls, bool placeInWebFolder) {
    if (!win || len(urls) == 0 || !LibraryIsAvailable()) {
        return;
    }
    i64 folderId = placeInWebFolder ? EnsureWebSitesCollectionId() : 0;
    i64 firstBookId = 0;
    int added = 0;
    for (Str url : urls) {
        if (!IsLibraryEligibleHttpUrl(url)) {
            // Still open in the browser if the caller passed an internal URL.
            WebBrowserOpenUrlAsNewTab(win, url, Str{});
            continue;
        }
        // Title starts empty — library shows full URL until document.title arrives.
        LibraryBook* book = LibraryStoreAddWebBook(LibraryGetStore(), url, StrL(""), UnixTimeMsNow());
        if (!book) {
            continue;
        }
        if (folderId > 0) {
            LibraryStorePlaceBook(LibraryGetStore(), book->id, 0, folderId, false);
        }
        // Bind before navigate so title/url sync lands on the correct library row.
        win->activeLibraryBookId = book->id;
        win->activeLibraryBookKind = (int)LibraryBookKind::Web;
        if (firstBookId == 0) {
            firstBookId = book->id;
        }
        WebBrowserOpenUrlAsNewTab(win, url, Str{});
        DeleteLibraryBook(book);
        added++;
    }
    RefreshLibraryPanels();
    if (firstBookId > 0) {
        win->activeLibraryBookId = firstBookId;
        win->activeLibraryBookKind = (int)LibraryBookKind::Web;
        WebBrowserShowPanel(win);
        SyncLibrarySelection(win);
    }
    if (added == 0 && len(urls) > 0) {
        // All URLs were internal / non-http — browser tabs may still have opened.
        WebBrowserShowPanel(win);
    } else if (added == 0) {
        MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("未识别到有效的网页链接。")), CWStrTemp(_TRA("添加网页")),
                    MB_OK | MB_ICONWARNING);
    }
}

void LibraryOpenWebUrlInBrowser(MainWindow* win, Str url) {
    if (!win || !url || !LibraryIsAvailable()) {
        return;
    }
    Vec<Str> urls;
    urls.Append(str::Dup(url));
    AddSitesToLibraryAndTabs(win, urls, true);
    for (Str u : urls) {
        str::Free(u);
    }
}

static void AddWebSitesPrompt(MainWindow* win) {
    if (!win || !LibraryIsAvailable()) {
        return;
    }
    Str text = PromptLibraryMultiline(win->hwndFrame, _TRA("添加网页"),
                                      _TRA("输入一个或多个网址（空格或换行分隔）"));
    if (!text) {
        return;
    }
    Vec<Str> urls;
    ParseSiteUrls(text, &urls);
    str::Free(text);
    AddSitesToLibraryAndTabs(win, urls, true);
    for (Str u : urls) {
        str::Free(u);
    }
}

static void AddWebSitesFromClipboard(MainWindow* win) {
    if (!win || !LibraryIsAvailable()) {
        return;
    }
    TempStr clip = LibraryClipboardTextTemp();
    if (!clip) {
        MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("粘贴板中没有文本。")), CWStrTemp(_TRA("添加网页从粘贴板")),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    Vec<Str> urls;
    ParseSiteUrls(clip, &urls);
    if (len(urls) == 0) {
        MessageBoxW(win->hwndFrame, CWStrTemp(_TRA("粘贴板中未检测到网页链接。")),
                    CWStrTemp(_TRA("添加网页从粘贴板")), MB_OK | MB_ICONINFORMATION);
        return;
    }
    AddSitesToLibraryAndTabs(win, urls, true);
    for (Str u : urls) {
        str::Free(u);
    }
}

static void AddMenu(MainWindow* win, VirtMouseEvent* ev) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kLibraryAddFolder, CWStrTemp(_TRA("新建文件夹")));
    AppendMenuW(menu, MF_STRING, kLibraryAddShelf, CWStrTemp(_TRA("新建书架")));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kLibraryAddPdf, CWStrTemp(_TRA("添加 PDF...")));
    AppendMenuW(menu, MF_STRING, kLibraryAddWeb, CWStrTemp(_TRA("添加网页...")));
    AppendMenuW(menu, MF_STRING, kLibraryAddWebClipboard, CWStrTemp(_TRA("添加网页从粘贴板")));
    AppendMenuW(menu, MF_STRING, kLibraryImportDir, CWStrTemp(_TRA("导入 PDF 目录...")));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kLibraryReplacePath, CWStrTemp(_TRA("替换路径前缀...")));
    Point pt = HwndClientToScreen(win->hwndLibraryBox, ev->pt);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, win->hwndFrame, nullptr);
    DestroyMenu(menu);
    if (cmd == kLibraryAddShelf) {
        if (CreateNamedCollection(win, 0, true)) RefreshLibraryPanels();
    } else if (cmd == kLibraryAddFolder) {
        if (CreateNamedCollection(win, 0, false)) RefreshLibraryPanels();
    } else if (cmd == kLibraryAddPdf) {
        AddPdfFiles(win);
    } else if (cmd == kLibraryAddWeb) {
        AddWebSitesPrompt(win);
    } else if (cmd == kLibraryAddWebClipboard) {
        AddWebSitesFromClipboard(win);
    } else if (cmd == kLibraryImportDir) {
        ImportPdfDirectory(win);
    } else if (cmd == kLibraryReplacePath) {
        ReplaceLibraryPathPrefix(win);
    }
}

void LayoutLibraryPanel(MainWindow* win) {
    if (!win || !win->libraryLayout || !win->hwndLibraryBox) return;
    Rect rc = HwndClientRect(win->hwndLibraryBox);
    if (rc.IsEmpty()) return;
    LayoutTreeToSize(win->hwndLibraryBox, win->libraryLayout, {rc.dx, rc.dy}, &win->libraryRoot);
}

static WNDPROC gLibraryBoxWndProc = nullptr;

static int LibraryLeftGutterDx() {
    return DpiScale(kLibraryLeftGutterDip);
}

static LRESULT CALLBACK LibraryBoxWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win) return CallWindowProcW(gLibraryBoxWndProc, hwnd, msg, wp, lp);
    // The library HWND covers the frame's left resize strip. Let the parent
    // see that edge so the cursor can become HTLEFT.
    if (msg == WM_NCHITTEST) {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.x >= 0 && pt.x < LibraryLeftGutterDx()) {
            return HTTRANSPARENT;
        }
    }
    LRESULT result = 0;
    result = TryReflectMessages(hwnd, msg, wp, lp);
    if (result) return result;
    if (VirtHostOnMessage(hwnd, win->libraryRoot, msg, wp, lp, result, ThemeControlBackgroundColor())) return result;
    if (msg == WM_SIZE) LayoutLibraryPanel(win);
    return CallWindowProcW(gLibraryBoxWndProc, hwnd, msg, wp, lp);
}

static VirtIconButton* HeaderButton(const char* svg, Str tooltip, const VirtMouseHandler& onClick) {
    auto* button = new VirtIconButton();
    button->pixmap = GetCachedPixmapForSvg(Str(svg), DpiScale(16), DpiScale(16));
    int pad = DpiScale(2);
    button->padding = Insets{0, pad, 0, pad};
    button->onClick = onClick;
    button->SetTooltip(tooltip);
    return button;
}

void CreateLibraryPanel(MainWindow* win) {
    DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    win->hwndLibraryBox = CreateWindowW(WC_STATICW, L"", style, 0, 0, gGlobalPrefs->libraryDx, 0, win->hwndFrame,
                                        nullptr, GetModuleHandleW(nullptr), nullptr);
    PlatformFont* labelFont = GetAppSidebarLabelFont();
    auto header = NewLabelWithClose(win->hwndLibraryBox, labelFont, MkFunc1(CloseLibrary, win));
    win->libraryLabel = header.label;
    header.label->SetText(_TRA("Library"));
    win->libraryAddButton = HeaderButton(gIconPlus, _TRA("Add to Library"), MkFunc1(AddMenu, win));
    // Insert + immediately before the close button so the search field sits
    // on the same row as the bookmarks search (no extra action row).
    if (len(header.box->children) > 0) {
        header.box->children.Pop();
    }
    header.box->AddChild(win->libraryAddButton);
    header.box->AddChild(header.closeBtn);

    auto* filter = new Edit();
    Edit::CreateArgs editArgs;
    editArgs.parent = win->hwndLibraryBox;
    editArgs.withBorder = true;
    editArgs.cueText = _TRA("Search books or paths");
    editArgs.font = GetAppFont();
    filter->Create(editArgs);
    filter->onTextChanged = MkFunc0(OnFilterChanged, win);
    win->libraryFilterEdit = filter;

    auto* tree = new TreeView();
    TreeView::CreateArgs treeArgs;
    treeArgs.parent = win->hwndLibraryBox;
    treeArgs.font = GetAppTreeFont();
    treeArgs.fullRowSelect = true;
    treeArgs.isRtl = IsUIRtl();
    tree->onContextMenu = MkFunc1Void(OnTreeContextMenu);
    tree->onGetTooltip = MkFunc1Void(OnTreeTooltip);
    tree->onCustomDraw = MkFunc1Void(OnLibraryCustomDraw);
    tree->onExpansionChanged = MkFunc0(OnLibraryTreeExpansionChanged, win);
    tree->preserveScrollOnExpand = true;
    tree->lazyChildren = true;
    tree->reuseRowsOnSetModel = true;
    tree->Create(treeArgs);
    LONG_PTR noDrag = GetWindowLongPtrW(tree->hwnd, GWL_STYLE);
    SetWindowLongPtrW(tree->hwnd, GWL_STYLE, noDrag | TVS_DISABLEDRAGDROP);
    // Keep the tree's client width stable while switching documents. The
    // library model changes the number of rows, and letting Windows add/remove
    // the vertical scrollbar changes the pane's right edge by one scrollbar
    // width, which makes the adjacent chrome appear to shift.
    LONG_PTR treeStyle = GetWindowLongPtrW(tree->hwnd, GWL_STYLE);
    SetWindowLongPtrW(tree->hwnd, GWL_STYLE, treeStyle | WS_VSCROLL);
    ShowScrollBar(tree->hwnd, SB_VERT, TRUE);
    win->libraryTreeView = tree;
    SetWindowSubclass(tree->hwnd, LibraryTreeSubclassProc, NextSubclassId(), (DWORD_PTR)win);

    auto* layout = new VBox();
    layout->alignCross = CrossAxisAlign::Stretch;
    layout->AddChild(header.box);
    layout->AddChild(filter);
    layout->AddChild(new Spacer(0, 2));
    layout->AddChild(tree, 1);
    win->libraryLayout = new Padding(layout, Insets{0, 0, 0, LibraryLeftGutterDx()});

    if (!gLibraryBoxWndProc) gLibraryBoxWndProc = (WNDPROC)GetWindowLongPtrW(win->hwndLibraryBox, GWLP_WNDPROC);
    SetWindowLongPtrW(win->hwndLibraryBox, GWLP_WNDPROC, (LONG_PTR)LibraryBoxWndProc);
    LoadLibraryTreeUiState(win);
    RefreshLibraryPanel(win);
    UpdateControlsColors(win);
}

void UpdateLibraryPanelIcons(MainWindow* win) {
    if (!win || !win->libraryAddButton) return;
    win->libraryAddButton->pixmap = GetCachedPixmapForSvg(Str(gIconPlus), DpiScale(16), DpiScale(16));
    if (win->hwndLibraryBox) HwndInvalidate(win->hwndLibraryBox);
}

void UpdateLibraryPanelText(MainWindow* win) {
    if (!win) return;
    if (win->libraryLabel) {
        win->libraryLabel->SetText(_TRA("Library"));
        win->libraryLabel->Invalidate();
    }
    if (win->libraryFilterEdit) {
        win->libraryFilterEdit->SetCue(_TRA("Search books or paths"));
    }
    if (win->libraryAddButton) {
        win->libraryAddButton->SetTooltip(_TRA("Add to Library"));
    }
    RefreshLibraryPanel(win);
}

void SetLibraryPanelVisible(MainWindow* win, bool visible) {
    if (!LibraryIsAvailable()) visible = false;
    gGlobalPrefs->showLibrary = visible;
    win->uiState.libraryVisible = visible;
    if (!visible && win->libraryTreeView && HwndIsFocused(win->libraryTreeView->hwnd)) HwndSetFocus(win->hwndFrame);
    SaveSettings();
    bool toc = win->CurrentTab() && win->CurrentTab()->showToc;
    logf("SetLibraryPanelVisible: visible=%d tocPref=%d\n", visible ? 1 : 0, toc ? 1 : 0);
    SetSidebarVisibility(win, toc, gGlobalPrefs->showFavorites);
}

static void AppendLibraryStatus(str::Builder& out, MainWindow* win) {
    WindowTab* tab = win ? win->CurrentTab() : nullptr;
    Str path = tab && tab->filePath ? tab->filePath : Str();
    out.Append(fmt("libraryVis=%d tocVis=%d showToc=%d libraryDx=%d sidebarDx=%d tabs=%d path=%s\n",
                   win && win->uiState.libraryVisible ? 1 : 0, win && win->uiState.tocVisible ? 1 : 0,
                   tab && tab->showToc ? 1 : 0, win ? win->libraryDx : 0, win ? win->sidebarDx : 0,
                   win ? win->TabCount() : 0, path ? path : StrL("")));
}

TempStr LibraryDbgControlTemp(Str action, Str a, Str b, int n1, int n2, int* exitCodeOut) {
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
        out.Append(StrL("OK "));
        AppendLibraryStatus(out, win);
        return finish(Str(), 0);
    }

    if (str::EqI(action, "show") || str::EqI(action, "hide")) {
        SetLibraryPanelVisible(win, str::EqI(action, "show"));
        out.Append(StrL("OK "));
        AppendLibraryStatus(out, win);
        return finish(Str(), win->uiState.libraryVisible == str::EqI(action, "show") ? 0 : 1);
    }

    if (str::EqI(action, "toc-show") || str::EqI(action, "toc-hide")) {
        bool want = str::EqI(action, "toc-show");
        SetSidebarVisibility(win, want, gGlobalPrefs->showFavorites);
        out.Append(StrL("OK "));
        AppendLibraryStatus(out, win);
        return finish(Str(), win->uiState.tocVisible == want ? 0 : 1);
    }

    if (str::EqI(action, "refresh")) {
        RefreshLibraryPanel(win);
        return finish(StrL("OK refreshed"), 0);
    }

    if (str::EqI(action, "open")) {
        if (!a) {
            return finish(StrL("ERROR open expects path"), 1);
        }
        logf("LibraryDbg: open '%s'\n", a);
        LoadArgs args(a, win);
        args.activateExisting = true;
        MainWindow* loaded = LoadDocument(&args);
        if (!loaded) {
            return finish(fmt("ERROR open-failed path=%s", a), 1);
        }
        out.Append(StrL("OK "));
        AppendLibraryStatus(out, win);
        return finish(Str(), 0);
    }

    if (str::EqI(action, "open-web")) {
        if (!a) {
            return finish(StrL("ERROR open-web expects url"), 1);
        }
        logf("LibraryDbg: open-web '%s'\n", a);
        LibraryOpenWebUrlInBrowser(win, a);
        out.Append(StrL("OK "));
        AppendLibraryStatus(out, win);
        return finish(Str(), win->uiState.webBrowserVisible ? 0 : 1);
    }

    if (str::EqI(action, "switch-tab")) {
        int nTabs = win->TabCount();
        if (n1 < 0 || n1 >= nTabs) {
            return finish(fmt("ERROR bad-tab idx=%d tabs=%d", n1, nTabs), 1);
        }
        logf("LibraryDbg: switch-tab %d\n", n1);
        ULONGLONG t0 = GetTickCount64();
        TabsSelect(win, n1);
        out.Append(fmt("OK idx=%d elapsedMs=%llu ", n1, GetTickCount64() - t0));
        AppendLibraryStatus(out, win);
        return finish(Str(), 0);
    }

    if (str::EqI(action, "tabs")) {
        out.Append(fmt("OK count=%d\n", win->TabCount()));
        for (int i = 0; i < win->TabCount(); i++) {
            WindowTab* tab = win->GetTab(i);
            Str path = tab && tab->filePath ? tab->filePath : StrL("");
            out.Append(fmt("tab idx=%d current=%d path=%s\n", i, tab == win->CurrentTab() ? 1 : 0, path));
        }
        return finish(Str(), 0);
    }

    if (str::EqI(action, "list")) {
        if (!LibraryIsAvailable()) {
            return finish(StrL("ERROR library-unavailable"), 1);
        }
        Vec<LibraryCollection*> cols = LibraryStoreGetCollections(LibraryGetStore());
        Vec<LibraryBook*> books =
            LibraryStoreGetBooks(LibraryGetStore(), LibraryBookScope::All, 0, LibrarySort::Title, Str());
        out.Append(fmt("OK collections=%d books=%d\n", len(cols), len(books)));
        for (LibraryCollection* c : cols) {
            out.Append(fmt("col id=%lld parent=%lld shelf=%d name=%s\n", c->id, c->parentId, c->isShelf ? 1 : 0,
                           c->name ? c->name : StrL("")));
        }
        for (LibraryBook* book : books) {
            out.Append(fmt("book id=%lld title=%s path=%s\n", book->id, book->title ? book->title : StrL(""),
                           book->path ? book->path : StrL("")));
        }
        DeleteLibraryCollections(cols);
        DeleteLibraryBooks(books);
        return finish(Str(), 0);
    }

    if (str::EqI(action, "new-shelf")) {
        if (!a) {
            return finish(StrL("ERROR new-shelf expects name"), 1);
        }
        LibraryCollection* created = LibraryStoreCreateCollection(LibraryGetStore(), 0, true, a);
        if (!created) {
            return finish(fmt("ERROR create-shelf name=%s err=%s", a, LibraryGetError()), 1);
        }
        i64 id = created->id;
        DeleteLibraryCollection(created);
        RefreshLibraryPanels();
        return finish(fmt("OK shelf=%lld name=%s", id, a), 0);
    }

    if (str::EqI(action, "place")) {
        if (n1 <= 0 || n2 < 0) {
            return finish(StrL("ERROR place expects bookId targetCollectionId"), 1);
        }
        bool ok = LibraryStorePlaceBook(LibraryGetStore(), n1, 0, n2, false);
        if (!ok) {
            return finish(fmt("ERROR place book=%d dest=%d err=%s", n1, n2, LibraryGetError()), 1);
        }
        RefreshLibraryPanels();
        return finish(fmt("OK placed book=%d dest=%d", n1, n2), 0);
    }

    if (str::EqI(action, "reorder")) {
        // n1=bookId, n2=targetBookId, a="after"|"before", b=collectionId (optional, default 0)
        if (n1 <= 0 || n2 <= 0) {
            return finish(StrL("ERROR reorder expects bookId targetBookId [after|before] [collectionId]"), 1);
        }
        bool insertAfter = a && str::EqI(a, "after");
        i64 collectionId = 0;
        if (b && b.s && b.s[0]) {
            collectionId = atoi(b.s);
        }
        bool ok = LibraryStoreReorderBook(LibraryGetStore(), n1, collectionId, n2, insertAfter);
        if (!ok) {
            return finish(fmt("ERROR reorder book=%d target=%d err=%s", n1, n2, LibraryGetError()), 1);
        }
        RefreshLibraryPanels();
        return finish(fmt("OK reordered book=%d target=%d after=%d col=%lld", n1, n2, insertAfter ? 1 : 0, collectionId),
                      0);
    }

    if (str::EqI(action, "click-burst")) {
        // a = comma-separated book ids, "clicked" back-to-back like fast mouse clicks
        auto* model = win->libraryTreeView ? (LibraryTreeModel*)win->libraryTreeView->treeModel : nullptr;
        if (!a || !model) {
            return finish(StrL("ERROR click-burst expects ids and a library tree"), 1);
        }
        gLibraryTimerOpens = 0;
        StrVec ids;
        Split(&ids, a, StrL(","), true);
        for (Str s : ids) {
            LibraryTreeItem* item = FindBookById(model->root, (i64)atoll(s.s));
            if (!item) {
                return finish(fmt("ERROR click-burst no book id=%s", s), 1);
            }
            ScheduleOpenLibraryItem(win, item);
        }
        return finish(fmt("OK clicks=%d", len(ids)), 0);
    }

    if (str::EqI(action, "click-stats")) {
        out.Append(fmt("OK opens=%d pending=%lld active=%lld aiApplied=%lld ", gLibraryTimerOpens,
                       win->libraryPendingOpenBookId, win->activeLibraryBookId, win->aiAppliedBookId));
        AppendLibraryStatus(out, win);
        return finish(Str(), 0);
    }

    if (str::EqI(action, "tag")) {
        // n1=bookId, n2=collectionId
        if (n1 <= 0 || n2 <= 0) {
            return finish(StrL("ERROR tag expects bookId collectionId"), 1);
        }
        if (!LibraryStoreAddBookTag(LibraryGetStore(), n1, n2)) {
            return finish(fmt("ERROR tag book=%d col=%d err=%s", n1, n2, LibraryGetError()), 1);
        }
        RefreshLibraryPanels();
        return finish(fmt("OK tagged book=%d col=%d", n1, n2), 0);
    }

    if (str::EqI(action, "new-folder")) {
        // a=name, b=parent collection id (optional, 0 = library root)
        if (!a) {
            return finish(StrL("ERROR new-folder expects name [parentId]"), 1);
        }
        i64 parentId = b && b.s && b.s[0] ? (i64)atoll(b.s) : 0;
        LibraryCollection* created = LibraryStoreCreateCollection(LibraryGetStore(), parentId, false, a);
        if (!created) {
            return finish(fmt("ERROR new-folder name=%s err=%s", a, LibraryGetError()), 1);
        }
        i64 id = created->id;
        DeleteLibraryCollection(created);
        RefreshLibraryPanels();
        return finish(fmt("OK folder=%lld parent=%lld", id, parentId), 0);
    }

    if (str::EqI(action, "retag")) {
        // n1=source folder, n2=target folder (0 = library root)
        if (n1 <= 0 || n2 < 0) {
            return finish(StrL("ERROR retag expects sourceCollectionId targetCollectionId"), 1);
        }
        if (!LibraryStoreRetagCollection(LibraryGetStore(), n1, n2)) {
            return finish(fmt("ERROR retag src=%d dst=%d err=%s", n1, n2, LibraryGetError()), 1);
        }
        RefreshLibraryPanels();
        return finish(fmt("OK retag src=%d dst=%d", n1, n2), 0);
    }

    if (str::EqI(action, "drag-folder")) {
        // a=source folder id, b="targetId:before|into|after"; drives the tree's real mouse handlers
        TreeView* tv = win->libraryTreeView;
        auto* model = tv ? (LibraryTreeModel*)tv->treeModel : nullptr;
        char zone[16]{};
        i64 dstId = 0;
        if (!a || !b || !model || sscanf_s(b.s, "%lld:%15s", &dstId, zone, (unsigned)sizeof(zone)) != 2) {
            return finish(StrL("ERROR drag-folder expects srcId 'dstId:before|into|after'"), 1);
        }
        if (!win->uiState.libraryVisible) {
            SetLibraryPanelVisible(win, true);
        }
        LibraryTreeItem* src = FindCollectionItemRec(model->root, (i64)atoll(a.s));
        LibraryTreeItem* dst = FindCollectionItemRec(model->root, dstId);
        if (!src || !dst) {
            return finish(StrL("ERROR drag-folder unknown folder"), 1);
        }
        Rect rs{}, rd{};
        for (LibraryTreeItem* it : {src, dst}) {
            HTREEITEM hi = tv->EnsureHandleByTreeItem((TreeItem)it);
            if (hi) {
                TreeView_EnsureVisible(tv->hwnd, hi);
            }
        }
        if (!tv->GetItemRect((TreeItem)src, false, rs) || !tv->GetItemRect((TreeItem)dst, false, rd)) {
            return finish(StrL("ERROR drag-folder rows not visible"), 1);
        }
        int y = rd.y + rd.dy / 2;
        if (str::EqI(zone, "before")) {
            y = rd.y + 1;
        } else if (str::EqI(zone, "after")) {
            y = rd.y + rd.dy - 2;
        }
        int sx = rs.x + 20, sy = rs.y + rs.dy / 2;
        SendMessageW(tv->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(sx, sy));
        SendMessageW(tv->hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(sx, sy + 6));
        SendMessageW(tv->hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(rd.x + 20, y));
        SendMessageW(tv->hwnd, WM_LBUTTONUP, 0, MAKELPARAM(rd.x + 20, y));
        return finish(fmt("OK dragged zone=%s", Str(zone)), 0);
    }

    if (str::EqI(action, "placements")) {
        // one line per placement: "place book=<id> col=<collectionId, 0 = root>"
        Vec<LibraryBook*> placed = LibraryStoreGetPlacedBooks(LibraryGetStore(), Str());
        out.Append(fmt("OK placements=%d\n", len(placed)));
        for (LibraryBook* book : placed) {
            out.Append(fmt("place book=%lld col=%lld\n", book->id, book->placementId));
        }
        DeleteLibraryBooks(placed);
        return finish(Str(), 0);
    }

    if (str::EqI(action, "seed")) {
        // a="folders:perFolder:rootBooks" (control args share slots, so pack them in one string)
        if (!gForTesting) {
            return finish(StrL("ERROR seed requires -for-testing"), 1);
        }
        int rootBooks = 0;
        n1 = n2 = 0;
        if (!a || sscanf_s(a.s, "%d:%d:%d", &n1, &n2, &rootBooks) < 2) {
            return finish(StrL("ERROR seed expects 'folders:perFolder[:rootBooks]'"), 1);
        }
        TimeStamp t0 = TimeGet();
        if (!LibraryStoreSeedForTesting(LibraryGetStore(), n1, n2, rootBooks)) {
            return finish(fmt("ERROR seed err=%s", LibraryGetError()), 1);
        }
        double seedMs = TimeSinceInMs(t0);
        return finish(fmt("OK seeded folders=%d perFolder=%d root=%d seedMs=%.0f", n1, n2, rootBooks, seedMs), 0);
    }

    if (str::EqI(action, "perf")) {
        // n1=pages to scroll (default 200). Reports tree build/populate/scroll/filter cost.
        TreeView* tv = win->libraryTreeView;
        if (!tv || !tv->hwnd) {
            return finish(StrL("ERROR no-tree"), 1);
        }
        if (!win->uiState.libraryVisible) {
            SetLibraryPanelVisible(win, true);
        }
        TimeStamp t0 = TimeGet();
        LibraryTreeModel* probe = BuildModel(win, FilterTextTemp(win));
        double buildMs = TimeSinceInMs(t0);
        delete probe;

        t0 = TimeGet();
        RefreshLibraryPanel(win);
        double refreshMs = TimeSinceInMs(t0);
        out.Append(fmt("refresh[%s] ", Str(gLastLibraryRefreshTiming)));
        int tvItems = (int)TreeView_GetCount(tv->hwnd);

        t0 = TimeGet();
        tv->ExpandAll();
        double expandAllMs = TimeSinceInMs(t0);
        int tvItemsExpanded = (int)TreeView_GetCount(tv->hwnd);

        int pages = n1 > 0 ? n1 : 200;
        SendMessageW(tv->hwnd, WM_VSCROLL, SB_TOP, 0);
        UpdateWindow(tv->hwnd);
        double scrollMaxMs = 0;
        t0 = TimeGet();
        for (int i = 0; i < pages; i++) {
            TimeStamp p0 = TimeGet();
            SendMessageW(tv->hwnd, WM_VSCROLL, SB_PAGEDOWN, 0);
            RedrawWindow(tv->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            double dt = TimeSinceInMs(p0);
            if (dt > scrollMaxMs) {
                scrollMaxMs = dt;
            }
        }
        double scrollMs = TimeSinceInMs(t0);

        t0 = TimeGet();
        SendMessageW(tv->hwnd, WM_VSCROLL, SB_BOTTOM, 0);
        RedrawWindow(tv->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        double jumpBottomMs = TimeSinceInMs(t0);

        int scrollBefore = GetScrollPos(tv->hwnd, SB_VERT);
        t0 = TimeGet();
        RefreshLibraryPanel(win);
        double refreshExpandedMs = TimeSinceInMs(t0);
        out.Append(fmt("refreshExpanded[%s] ", Str(gLastLibraryRefreshTiming)));
        int scrollAfter = GetScrollPos(tv->hwnd, SB_VERT);
        out.Append(fmt("refreshExpandedMs=%.1f scrollKept=%d ", refreshExpandedMs, scrollBefore == scrollAfter ? 1 : 0));

        double filterMs = -1;
        double filterBroadMs = -1;
        if (win->libraryFilterEdit) {
            Edit* edit = win->libraryFilterEdit;
            auto savedOnChanged = edit->onTextChanged;
            edit->onTextChanged = {};
            edit->SetText(StrL("book-0099"));
            t0 = TimeGet();
            RefreshLibraryPanel(win);
            filterMs = TimeSinceInMs(t0);
            out.Append(fmt("filter[%s] ", Str(gLastLibraryRefreshTiming)));
            edit->SetText(StrL("book"));
            t0 = TimeGet();
            RefreshLibraryPanel(win);
            filterBroadMs = TimeSinceInMs(t0);
            out.Append(fmt("broad[%s] ", Str(gLastLibraryRefreshTiming)));
            edit->SetText(Str());
            edit->onTextChanged = savedOnChanged;
            RefreshLibraryPanel(win);
        }
        tv->CollapseAll();
        RefreshLibraryPanel(win);
        return finish(fmt("OK buildMs=%.1f refreshMs=%.1f tvItems=%d expandAllMs=%.1f tvItemsExpanded=%d "
                          "pages=%d scrollMs=%.1f scrollAvgMs=%.2f scrollMaxMs=%.1f jumpBottomMs=%.1f filterMs=%.1f "
                          "filterBroadMs=%.1f",
                          buildMs, refreshMs, tvItems, expandAllMs, tvItemsExpanded, pages, scrollMs,
                          pages > 0 ? scrollMs / pages : 0.0, scrollMaxMs, jumpBottomMs, filterMs, filterBroadMs),
                      0);
    }

    if (str::EqI(action, "stress-switch")) {
        Vec<int> docTabs;
        int nTabs = win->TabCount();
        for (int i = 0; i < nTabs; i++) {
            WindowTab* tab = win->GetTab(i);
            if (tab && tab->IsDocLoaded()) {
                docTabs.Append(i);
            }
        }
        if (len(docTabs) < 2) {
            return finish(fmt("ERROR need-doc-tabs have=%d", len(docTabs)), 1);
        }
        int rounds = n1 > 0 ? n1 : 20;
        if (rounds > 500) {
            rounds = 500;
        }
        int maxMs = 0;
        int hangAt = -1;
        logf("LibraryDbg: stress-switch rounds=%d docTabs=%d libraryVis=%d\n", rounds, len(docTabs),
             win->uiState.libraryVisible ? 1 : 0);
        for (int i = 0; i < rounds; i++) {
            ULONGLONG t0 = GetTickCount64();
            TabsSelect(win, docTabs[i % len(docTabs)]);
            int dt = (int)(GetTickCount64() - t0);
            if (dt > maxMs) {
                maxMs = dt;
            }
            if (dt > 5000) {
                hangAt = i;
                logf("LibraryDbg: stress-switch hang i=%d ms=%d\n", i, dt);
                break;
            }
        }
        out.Append(fmt("OK rounds=%d maxMs=%d hangAt=%d ", rounds, maxMs, hangAt));
        AppendLibraryStatus(out, win);
        return finish(Str(), hangAt >= 0 ? 1 : 0);
    }

    return finish(fmt("ERROR unknown-action action=%s", action), 1);
}
