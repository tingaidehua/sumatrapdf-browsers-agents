/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/JsonParser.h"
#include "base/Win.h"

#include "gui/Dpi.h"
#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/VirtCtrl.h"
#include "gui/win/WinGui.h"
#include "gui/win/WebView.h"

#include "Settings.h"
#include "Theme.h"
#include "DarkMode_win.h"
#include "SumatraConfig.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "Translations.h"
#include "WebPanel.h"
#include "ScriptManager.h"
#include "SumatraLog.h"

enum {
    kSmBtnSave = 1001,
    kSmBtnFormat,
    kSmBtnRun,
    kSmBtnReload,
    kSmBtnFolder,
};

struct ScriptEntry {
    Str category;
    Str title;
    Str relPath;
    Str args;
    bool canRun = false;
    bool editable = false;
};

struct ScriptManagerWnd : WindowBase {
    MainWindow* owner = nullptr;
    HWND hwndTree = nullptr;
    HWND hwndStatus = nullptr;
    HWND hwndBtnSave = nullptr;
    HWND hwndBtnFormat = nullptr;
    HWND hwndBtnRun = nullptr;
    HWND hwndBtnReload = nullptr;
    HWND hwndBtnFolder = nullptr;
    WebviewWnd* editor = nullptr;

    Str scriptsDir;
    Vec<ScriptEntry> entries;
    int selected = -1;
    bool editorReady = false;
    bool pendingPush = false;

    ~ScriptManagerWnd() override;

    bool Create(MainWindow* win);
    void LayoutChildren();
    void LoadManifest();
    void BuildTree();
    void SelectIndex(int idx);
    void PushToEditor();
    void SetStatus(Str msg);
    TempStr AbsPathTemp(Str rel) const;
    TempStr DetectLangTemp(Str rel) const;
    void DoSave(Str content);
    void DoRun();
    void OnCommand(WindowBase::CommandEvent* ev);
    void OnSize(WindowBase::SizeEvent* ev);
};

static ScriptManagerWnd* gScriptManager = nullptr;

ScriptManagerWnd::~ScriptManagerWnd() {
    if (gScriptManager == this) {
        gScriptManager = nullptr;
    }
    delete editor;
    editor = nullptr;
    for (ScriptEntry& e : entries) {
        str::Free(e.category);
        str::Free(e.title);
        str::Free(e.relPath);
        str::Free(e.args);
    }
    str::FreePtr(&scriptsDir);
}

static void ScriptManagerOnClose(WindowBase::CloseEvent* ev) {
    delete (ScriptManagerWnd*)ev->e->self;
}

TempStr ScriptManagerWnd::AbsPathTemp(Str rel) const {
    return path::JoinTemp(scriptsDir, rel);
}

TempStr ScriptManagerWnd::DetectLangTemp(Str rel) const {
    if (str::EndsWithI(rel, StrL(".json"))) {
        return StrL("json");
    }
    if (str::EndsWithI(rel, StrL(".mjs")) || str::EndsWithI(rel, StrL(".js"))) {
        return StrL("js");
    }
    return StrL("text");
}

void ScriptManagerWnd::SetStatus(Str msg) {
    if (hwndStatus) {
        HwndSetText(hwndStatus, msg ? msg : StrL(""));
    }
}

void ScriptManagerWnd::LoadManifest() {
    for (ScriptEntry& e : entries) {
        str::Free(e.category);
        str::Free(e.title);
        str::Free(e.relPath);
        str::Free(e.args);
    }
    entries.Clear();

    TempStr manPath = path::JoinTemp(scriptsDir, StrL("manifest.json"));
    Str data = file::ReadFile(manPath);
    if (!data) {
        ScriptEntry e{};
        e.category = str::Dup(StrL("NotebookLM"));
        e.title = str::Dup(StrL("配置 config.json"));
        e.relPath = str::Dup(StrL("config.json"));
        e.editable = true;
        entries.Append(e);
        e = {};
        e.category = str::Dup(StrL("NotebookLM"));
        e.title = str::Dup(StrL("添加 PDF"));
        e.relPath = str::Dup(StrL("notebooklm-add.mjs"));
        e.canRun = true;
        entries.Append(e);
        return;
    }

    struct ManState {
        ScriptManagerWnd* self = nullptr;
        Str curCat;
        ScriptEntry cur{};
        bool inItem = false;
    } st;
    st.self = this;

    auto onVal = [](ManState* s, json::Value* v) {
        TempStr p = json::PathFormatTemp(v->path);
        if (!p) {
            return;
        }
        if (str::EndsWithI(p, StrL("/title")) && str::ContainsI(p, StrL("/categories")) &&
            !str::ContainsI(p, StrL("/items"))) {
            str::ReplaceWithCopy(&s->curCat, v->value);
            return;
        }
        if (!str::ContainsI(p, StrL("/items"))) {
            return;
        }
        if (str::EndsWithI(p, StrL("/title"))) {
            if (s->inItem && s->cur.relPath) {
                s->cur.category = str::Dup(s->curCat);
                s->self->entries.Append(s->cur);
                s->cur = {};
            }
            s->inItem = true;
            str::ReplaceWithCopy(&s->cur.title, v->value);
        } else if (str::EndsWithI(p, StrL("/path"))) {
            str::ReplaceWithCopy(&s->cur.relPath, v->value);
        } else if (str::EndsWithI(p, StrL("/args"))) {
            str::ReplaceWithCopy(&s->cur.args, v->value);
        } else if (str::EndsWithI(p, StrL("/run"))) {
            s->cur.canRun = str::EqI(v->value, StrL("true")) || str::Eq(v->value, StrL("1"));
        } else if (str::EndsWithI(p, StrL("/editable"))) {
            s->cur.editable = str::EqI(v->value, StrL("true")) || str::Eq(v->value, StrL("1"));
        }
    };
    json::Parse(data, MkFunc1<ManState, json::Value*>(onVal, &st));
    if (st.inItem && st.cur.relPath) {
        st.cur.category = str::Dup(st.curCat);
        entries.Append(st.cur);
        st.cur = {};
    }
    str::Free(st.curCat);
    str::Free(st.cur.title);
    str::Free(st.cur.relPath);
    str::Free(st.cur.args);
    str::Free(data);
}

void ScriptManagerWnd::BuildTree() {
    if (!hwndTree) {
        return;
    }
    TreeView_DeleteAllItems(hwndTree);
    HTREEITEM lastCat = nullptr;
    Str lastCatName = {};
    for (int i = 0; i < entries.len; i++) {
        ScriptEntry& e = entries[i];
        if (!lastCat || !str::Eq(lastCatName, e.category)) {
            TVINSERTSTRUCTW is{};
            is.hParent = TVI_ROOT;
            is.hInsertAfter = TVI_LAST;
            is.item.mask = TVIF_TEXT | TVIF_PARAM;
            is.item.pszText = (LPWSTR)CWStrTemp(e.category ? e.category : StrL("Scripts"));
            is.item.lParam = -1;
            lastCat = TreeView_InsertItem(hwndTree, &is);
            lastCatName = e.category;
            if (lastCat) {
                TreeView_Expand(hwndTree, lastCat, TVE_EXPAND);
            }
        }
        TVINSERTSTRUCTW is{};
        is.hParent = lastCat;
        is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT | TVIF_PARAM;
        is.item.pszText = (LPWSTR)CWStrTemp(e.title ? e.title : e.relPath);
        is.item.lParam = i;
        TreeView_InsertItem(hwndTree, &is);
    }
}

void ScriptManagerWnd::PushToEditor() {
    if (!editor || selected < 0 || selected >= entries.len) {
        pendingPush = true;
        return;
    }
    if (!editorReady) {
        pendingPush = true;
        return;
    }
    ScriptEntry& e = entries[selected];
    // Only config.json (or manifest editable:true) may be saved — scripts stay read-only.
    bool editable = e.editable || str::EqI(path::GetBaseNameTemp(e.relPath), StrL("config.json"));
    TempStr abs = AbsPathTemp(e.relPath);
    Str text = file::ReadFile(abs);
    if (!text) {
        text = str::Dup(StrL(""));
        SetStatus(fmt(_TRA("文件不存在: %s").s, e.relPath));
    } else if (editable) {
        SetStatus(fmt(_TRA("已加载 %s（可编辑，保存后立即生效）").s, e.relPath));
    } else {
        SetStatus(fmt(_TRA("已加载 %s（只读 — 仅配置可改）").s, e.relPath));
    }
    TempStr lang = DetectLangTemp(e.relPath);
    TempStr js = fmt("if(window.setContent)window.setContent(\"%s\",\"%s\",\"%s\",%s,%s);",
                     json::EscapeStrTemp(text), json::EscapeStrTemp(e.relPath), json::EscapeStrTemp(lang),
                     e.canRun ? StrL("true") : StrL("false"), editable ? StrL("true") : StrL("false"));
    editor->Eval(js);
    EnableWindow(hwndBtnRun, e.canRun ? TRUE : FALSE);
    EnableWindow(hwndBtnSave, editable ? TRUE : FALSE);
    EnableWindow(hwndBtnFormat, editable ? TRUE : FALSE);
    str::Free(text);
    pendingPush = false;
}

void ScriptManagerWnd::SelectIndex(int idx) {
    if (idx < 0 || idx >= entries.len) {
        return;
    }
    selected = idx;
    PushToEditor();
}

void ScriptManagerWnd::DoSave(Str content) {
    if (selected < 0 || selected >= entries.len) {
        return;
    }
    ScriptEntry& e = entries[selected];
    bool editable = e.editable || str::EqI(path::GetBaseNameTemp(e.relPath), StrL("config.json"));
    if (!editable) {
        SetStatus(_TRA("脚本只读，仅配置可保存"));
        return;
    }
    TempStr abs = AbsPathTemp(e.relPath);
    dir::CreateAll(path::GetDirTemp(abs));
    bool ok = file::WriteFile(abs, content ? content : StrL(""));
    if (!ok) {
        SetStatus(_TRA("保存失败"));
        MessageBoxWarning(hwnd, _TRA("无法写入配置文件。"), _TRA("脚本管理"));
        return;
    }
    SetStatus(fmt(_TRA("已保存 %s — 下次运行立即生效").s, e.relPath));
}

void ScriptManagerWnd::DoRun() {
    if (selected < 0 || selected >= entries.len) {
        return;
    }
    ScriptEntry& e = entries[selected];
    if (!e.canRun) {
        SetStatus(_TRA("此文件不可直接运行"));
        return;
    }
    WebPanelSpawnScript(e.relPath, e.args);
    SetStatus(fmt(_TRA("已启动: node %s %s").s, e.relPath, e.args ? e.args : StrL("")));
}

static void OnEditorJsCall(void* ctx, Str id, Str method, Str paramsJson) {
    auto* sm = (ScriptManagerWnd*)ctx;
    if (!sm || !sm->editor) {
        return;
    }
    if (str::Eq(method, "hostSave")) {
        Str content = {};
        auto grab = [](Str* out, json::Value* v) {
            if (v->type == json::Type::String && json::PathMatch(v->path, StrL("i0"))) {
                *out = str::Dup(v->value);
                v->stop = true;
            }
        };
        json::Parse(paramsJson, MkFunc1<Str, json::Value*>(grab, &content));
        sm->DoSave(content);
        str::Free(content);
        sm->editor->Resolve(id, 0, StrL("true"));
        return;
    }
    if (str::Eq(method, "hostRun")) {
        sm->DoRun();
        sm->editor->Resolve(id, 0, StrL("true"));
        return;
    }
    sm->editor->Resolve(id, 1, StrL("\"unknown method\""));
}

static void OnEditorJsNotify(void* ctx, Str method, Str) {
    auto* sm = (ScriptManagerWnd*)ctx;
    if (!sm) {
        return;
    }
    if (str::Eq(method, "editorReady")) {
        sm->editorReady = true;
        if (sm->pendingPush || sm->selected >= 0) {
            sm->PushToEditor();
        }
    }
}

static LRESULT CALLBACK ScriptTreeSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* sm = (ScriptManagerWnd*)data;
    if (msg == WM_NOTIFY) {
        auto* hdr = (LPNMHDR)lp;
        if (hdr && hdr->code == TVN_SELCHANGEDW) {
            auto* nmtv = (LPNMTREEVIEWW)lp;
            int idx = (int)nmtv->itemNew.lParam;
            if (idx >= 0) {
                sm->SelectIndex(idx);
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void ScriptManagerWnd::LayoutChildren() {
    if (!hwnd) {
        return;
    }
    Rect rc = HwndClientRect(hwnd);
    int pad = DpiScale(8);
    int barH = DpiScale(32);
    int statusH = DpiScale(22);
    int treeW = DpiScale(260);
    int bw = DpiScale(72);
    int x = pad;
    int y = pad;
    auto place = [&](HWND h, int w) {
        if (h) {
            SetWindowPos(h, nullptr, x, y, w, barH, SWP_NOZORDER);
        }
        x += w + pad;
    };
    place(hwndBtnSave, bw);
    place(hwndBtnFormat, bw);
    place(hwndBtnRun, bw);
    place(hwndBtnReload, bw);
    place(hwndBtnFolder, DpiScale(96));

    int contentY = pad + barH + pad;
    int contentH = rc.dy - contentY - statusH - pad;
    if (contentH < 40) {
        contentH = 40;
    }
    if (hwndTree) {
        SetWindowPos(hwndTree, nullptr, pad, contentY, treeW, contentH, SWP_NOZORDER);
    }
    int edX = pad + treeW + pad;
    int edW = rc.dx - edX - pad;
    if (edW < 40) {
        edW = 40;
    }
    if (editor && editor->hwnd) {
        SetWindowPos(editor->hwnd, nullptr, edX, contentY, edW, contentH, SWP_NOZORDER);
        editor->UpdateWebviewSize();
    }
    if (hwndStatus) {
        SetWindowPos(hwndStatus, nullptr, pad, rc.dy - statusH, rc.dx - 2 * pad, statusH - 2, SWP_NOZORDER);
    }
}

void ScriptManagerWnd::OnSize(WindowBase::SizeEvent*) {
    LayoutChildren();
}

void ScriptManagerWnd::OnCommand(WindowBase::CommandEvent* ev) {
    if (!ev) {
        return;
    }
    int id = LOWORD(ev->wparam);
    switch (id) {
        case kSmBtnSave:
            if (editor && editorReady) {
                editor->Eval(StrL("if(window.__sumatra__)window.__sumatra__.call('hostSave', window.getContent());"));
            }
            ev->didHandle = true;
            break;
        case kSmBtnFormat:
            if (editor && editorReady) {
                editor->Eval(StrL("if(window.formatDocument)window.formatDocument();"));
            }
            ev->didHandle = true;
            break;
        case kSmBtnRun:
            if (editor && editorReady) {
                editor->Eval(StrL(
                    "if(window.__sumatra__)window.__sumatra__.call('hostSave', window.getContent())"
                    ".then(function(){return window.__sumatra__.call('hostRun');});"));
            } else {
                DoRun();
            }
            ev->didHandle = true;
            break;
        case kSmBtnReload:
            LoadManifest();
            BuildTree();
            if (selected >= 0) {
                PushToEditor();
            }
            SetStatus(_TRA("已从磁盘重新加载"));
            ev->didHandle = true;
            break;
        case kSmBtnFolder:
            if (scriptsDir) {
                SumatraOpenPathInDefaultFileManager(scriptsDir);
            }
            ev->didHandle = true;
            break;
    }
}

static LRESULT CALLBACK ScriptMgrFrameSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* sm = (ScriptManagerWnd*)data;
    if (msg == WM_NOTIFY && sm) {
        auto* hdr = (LPNMHDR)lp;
        if (hdr && hdr->hwndFrom == sm->hwndTree && hdr->code == TVN_SELCHANGEDW) {
            auto* nmtv = (LPNMTREEVIEWW)lp;
            int idx = (int)nmtv->itemNew.lParam;
            if (idx >= 0) {
                sm->SelectIndex(idx);
            }
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

bool ScriptManagerWnd::Create(MainWindow* win) {
    owner = win;
    scriptsDir = str::Dup(ScriptsWebviewDirTemp());
    TempStr cfgPath = path::JoinTemp(scriptsDir, StrL("config.json"));
    if (!file::Exists(cfgPath)) {
        file::WriteFile(cfgPath, StrL("{\n  \"sourcesPerNotebook\": 50,\n  \"maxNotebooks\": 40,\n"
                                      "  \"notebookNamePrefix\": \"SumatraPDF\",\n"
                                      "  \"cdpPreferredPort\": 9224,\n  \"uploadMaxMb\": 200\n}\n"));
    }

    CreateCustomArgs cargs;
    cargs.title = _TRA("脚本管理");
    cargs.style = WS_OVERLAPPEDWINDOW;
    cargs.visible = false;
    cargs.icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(GetAppIconID()));
    cargs.bgColor = DarkModeDialogBgColor();
    closeOnEsc = true;
    onClose = MkFunc1Void(ScriptManagerOnClose);
    onSize = MkMethod1<ScriptManagerWnd, WindowBase::SizeEvent*, &ScriptManagerWnd::OnSize>(this);
    onCommand = MkMethod1<ScriptManagerWnd, WindowBase::CommandEvent*, &ScriptManagerWnd::OnCommand>(this);
    CreateCustom(cargs);
    if (!hwnd) {
        return false;
    }
    if (win && win->hwndFrame) {
        SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, (LONG_PTR)win->hwndFrame);
    }
    SetWindowSubclass(hwnd, ScriptMgrFrameSubclass, 1, (DWORD_PTR)this);

    auto mkBtn = [&](int id, Str text) -> HWND {
        return CreateWindowW(WC_BUTTONW, CWStrTemp(text), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    };
    hwndBtnSave = mkBtn(kSmBtnSave, _TRA("保存"));
    hwndBtnFormat = mkBtn(kSmBtnFormat, _TRA("格式化"));
    hwndBtnRun = mkBtn(kSmBtnRun, _TRA("运行"));
    hwndBtnReload = mkBtn(kSmBtnReload, _TRA("刷新"));
    hwndBtnFolder = mkBtn(kSmBtnFolder, _TRA("打开目录"));

    hwndTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                               WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                                   TVS_SHOWSELALWAYS | TVS_FULLROWSELECT,
                               0, 0, 0, 0, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowSubclass(hwndTree, ScriptTreeSubclass, 1, (DWORD_PTR)this);

    hwndStatus = CreateWindowW(WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hwnd,
                               nullptr, GetModuleHandleW(nullptr), nullptr);

    if (HasWebView()) {
        editor = new WebviewWnd();
        editor->events.ctx = this;
        editor->events.jsCall = OnEditorJsCall;
        editor->events.jsNotify = OnEditorJsNotify;
        editor->dataDir = str::Dup(path::JoinTemp(scriptsDir, StrL("manager\\webview2")));
        editor->desiredVisible = true;
        editor->enableDevTools = true;
        CreateWebViewArgs wvArgs;
        wvArgs.parent = hwnd;
        wvArgs.pos = Rect(0, 0, 100, 100);
        editor->Create(wvArgs);

        TempStr editorHtml = path::JoinTemp(scriptsDir, StrL("manager\\editor.html"));
        TempStr repoRoot = FindRepoScriptsWebviewTemp();
        if (!file::Exists(editorHtml) && repoRoot &&
            file::Exists(path::JoinTemp(repoRoot, StrL("manager\\editor.html")))) {
            dir::CreateAll(path::GetDirTemp(editorHtml));
            file::Copy(editorHtml, path::JoinTemp(repoRoot, StrL("manager\\editor.html")), false);
            file::Copy(path::JoinTemp(scriptsDir, StrL("manifest.json")),
                       path::JoinTemp(repoRoot, StrL("manifest.json")), false);
            if (!file::Exists(cfgPath)) {
                file::Copy(cfgPath, path::JoinTemp(repoRoot, StrL("config.json")), false);
            }
        }
        if (file::Exists(editorHtml)) {
            TempStr norm = str::DupTemp(editorHtml);
            str::TransCharsInPlace(norm, StrL("\\"), StrL("/"));
            // Drive letter path → file:///C:/...
            TempStr url = fmt("file:///%s", norm);
            editor->Navigate(url);
        } else {
            editor->SetHtml(StrL("<html><body style='background:#1e1e1e;color:#ccc;font:13px Consolas'>"
                                 "缺少 manager/editor.html</body></html>"));
            editorReady = true;
        }
    } else {
        SetStatus(_TRA("WebView2 不可用，无法加载脚本编辑器"));
    }

    LoadManifest();
    BuildTree();
    if (entries.len > 0) {
        selected = 0;
        pendingPush = true;
    }

    ResizeHwndToClientArea(hwnd, DpiScale(980), DpiScale(640), false);
    LayoutChildren();
    HwndCenterDialog(hwnd, win ? win->hwndFrame : nullptr);
    DarkModeApplyToWindowAndEraseBg(hwnd);
    SetIsVisible(true);
    SetStatus(fmt(_TRA("脚本目录: %s").s, scriptsDir));
    return true;
}

void ShowScriptManagerDialog(MainWindow* win) {
    if (gScriptManager && gScriptManager->hwnd) {
        SetForegroundWindow(gScriptManager->hwnd);
        return;
    }
    auto* dlg = new ScriptManagerWnd();
    gScriptManager = dlg;
    if (!dlg->Create(win)) {
        delete dlg;
        gScriptManager = nullptr;
    }
}
