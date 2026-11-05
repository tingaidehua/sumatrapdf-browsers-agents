/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/File.h"
#include "base/Win.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "Translations.h"
#include "AppTools.h"
#include "WebPanel.h"
#include "DevInfo.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

namespace {

constexpr int kAiCdpPort = 9224;
constexpr int kWebCdpPort = 9225;
constexpr int kTcpTimeoutMs = 200;
constexpr int kHttpTimeoutMs = 600;

struct CdpProbe {
    int port = 0;
    bool tcpOk = false;
    bool httpOk = false;
    int elapsedMs = 0;
    Str summary; // owned
    Str error;   // owned
};

void FreeCdpProbe(CdpProbe* p) {
    if (!p) {
        return;
    }
    str::FreePtr(&p->summary);
    str::FreePtr(&p->error);
}

static bool EnsureWsa() {
    static bool inited = false;
    static bool ok = false;
    if (!inited) {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        inited = true;
    }
    return ok;
}

static bool TcpPortOpen(int port, int timeoutMs, int* elapsedMsOut) {
    ULONGLONG t0 = GetTickCount64();
    auto finish = [&](bool ok) -> bool {
        if (elapsedMsOut) {
            *elapsedMsOut = (int)(GetTickCount64() - t0);
        }
        return ok;
    };
    if (!EnsureWsa() || port <= 0 || port > 65535) {
        return finish(false);
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return finish(false);
    }
    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u16)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connect(s, (sockaddr*)&addr, sizeof(addr));
    fd_set wset, eset;
    FD_ZERO(&wset);
    FD_ZERO(&eset);
    FD_SET(s, &wset);
    FD_SET(s, &eset);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int sel = select(0, nullptr, &wset, &eset, &tv);
    bool ok = false;
    if (sel > 0 && FD_ISSET(s, &wset)) {
        int soErr = 0;
        int soLen = sizeof(soErr);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soErr, &soLen) == 0 && soErr == 0) {
            ok = true;
        }
    }
    closesocket(s);
    return finish(ok);
}

static bool HttpGetJsonVersion(int port, str::Builder* bodyOut, Str* errOut) {
    HINTERNET session =
        WinHttpOpen(L"SumatraPDF-CDP", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        if (errOut) {
            *errOut = str::Dup(fmt("WinHttpOpen %u", GetLastError()));
        }
        return false;
    }
    WinHttpSetTimeouts(session, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs);
    HINTERNET conn = WinHttpConnect(session, L"127.0.0.1", (INTERNET_PORT)port, 0);
    if (!conn) {
        if (errOut) {
            *errOut = str::Dup(fmt("connect %u", GetLastError()));
        }
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET req =
        WinHttpOpenRequest(conn, L"GET", L"/json/version", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) {
        if (errOut) {
            *errOut = str::Dup(fmt("OpenRequest %u", GetLastError()));
        }
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        if (errOut) {
            *errOut = str::Dup(fmt("request %u", GetLastError()));
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                        &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        if (errOut) {
            *errOut = str::Dup(fmt("HTTP %u", status));
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }
    char chunk[2048];
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) {
            break;
        }
        DWORD toRead = avail > sizeof(chunk) ? (DWORD)sizeof(chunk) : avail;
        DWORD nRead = 0;
        if (!WinHttpReadData(req, chunk, toRead, &nRead) || nRead == 0) {
            break;
        }
        // Keep only printable ASCII / UTF-8-ish text for the dialog.
        for (DWORD i = 0; i < nRead; i++) {
            u8 c = (u8)chunk[i];
            if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c != 0x7f) || c >= 0x80) {
                if (bodyOut) {
                    bodyOut->AppendChar((char)c);
                }
            }
        }
        if (bodyOut && len(*bodyOut) > 4000) {
            break;
        }
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return true;
}

CdpProbe ProbeCdp(int port) {
    CdpProbe r;
    r.port = port;
    ULONGLONG t0 = GetTickCount64();
    int tcpMs = 0;
    r.tcpOk = TcpPortOpen(port, kTcpTimeoutMs, &tcpMs);
    if (!r.tcpOk) {
        r.elapsedMs = (int)(GetTickCount64() - t0);
        r.error = str::Dup(fmt("端口无监听 (%d ms)", tcpMs));
        return r;
    }
    str::Builder body;
    Str err{};
    r.httpOk = HttpGetJsonVersion(port, &body, &err);
    r.elapsedMs = (int)(GetTickCount64() - t0);
    if (!r.httpOk) {
        r.error = err ? err : str::Dup(StrL("HTTP 失败"));
        r.summary = str::Dup(fmt("TCP 通，HTTP 失败: %s", r.error));
        return r;
    }
    // Don't parse JSON fields into the dialog — WinHTTP body can include odd bytes
    // that confuse MessageBox/file display. Status + known endpoints are enough.
    r.summary = str::Dup(StrL("HTTP 200  /json/version + /json/list"));
    return r;
}

TempStr FormatBlockTemp(Str label, int port, const CdpProbe& p) {
    str::Builder sb;
    sb.Append(fmt("[%s] port %d\n", label, port));
    if (p.httpOk) {
        sb.Append(fmt("  状态: 有效  (%d ms)\n", p.elapsedMs));
        if (p.summary) {
            sb.Append(fmt("  %s\n", p.summary));
        }
        sb.Append(fmt("  http://127.0.0.1:%d/json/list\n", port));
    } else if (p.tcpOk) {
        sb.Append(fmt("  状态: TCP 通 / HTTP 无效  (%d ms)\n", p.elapsedMs));
        sb.Append(fmt("  %s\n", p.error ? p.error : StrL("")));
    } else {
        sb.Append(fmt("  状态: 无效  (%d ms)\n", p.elapsedMs));
        sb.Append(fmt("  %s\n", p.error ? p.error : StrL("")));
        sb.Append(StrL("  启动后会自动拉起 CDP；若仍无效请重启 exe。\n"));
    }
    sb.AppendChar('\n');
    return ToStrTemp(sb);
}

} // namespace

TempStr BuildDevInfoTextTemp(MainWindow* win) {
    int aiPort = (win && win->webPanelCdpPort > 0) ? win->webPanelCdpPort : kAiCdpPort;
    int webPort = kWebCdpPort;

    CdpProbe ai = ProbeCdp(aiPort);
    CdpProbe web = ProbeCdp(webPort);

    str::Builder sb;
    sb.Append(StrL("SumatraPDF 开发信息\n"));
    sb.Append(StrL("==================\n\n"));
    sb.Append(FormatBlockTemp(StrL("AI 栏"), aiPort, ai));
    sb.Append(FormatBlockTemp(StrL("Web 栏"), webPort, web));
    sb.Append(StrL("自动化（打开 exe 即就绪，无需 node）\n"));
    sb.Append(StrL("  CDP AI : http://127.0.0.1:9224\n"));
    sb.Append(StrL("  CDP Web: http://127.0.0.1:9225\n"));
    sb.Append(StrL("  管道   : \\\\.\\pipe\\sumatrapdf-ai\n"));
    if (win) {
        sb.Append(fmt("\nAI tabs=%d active=%d | Web tabs=%d active=%d\n", len(win->webPanelTabs),
                      win->webPanelActiveTab, len(win->webBrowserTabs), win->webBrowserActiveTab));
        sb.Append(fmt("CDP keepers: AI=%d Web=%d\n", win->webPanelCdpKeeper ? 1 : 0,
                      win->webBrowserCdpKeeper ? 1 : 0));
    }
    FreeCdpProbe(&ai);
    FreeCdpProbe(&web);
    return ToStrTemp(sb);
}

void ShowDevInfoDialog(MainWindow* win) {
    logf("ShowDevInfoDialog: begin\n");
    // Make sure keepers exist before probing (first-click race after cold start).
    if (win) {
        EnsureWebViewCdpReady(win);
    }
    TempStr text = BuildDevInfoTextTemp(win);
    logf("ShowDevInfoDialog: text ready len=%d\n", text ? text.len : 0);
    TempStr path = GetPathInAppDataDirTemp(StrL("WebPanel\\bridge\\dev-info.txt"));
    if (path) {
        dir::CreateAll(path::GetDirTemp(path));
        file::WriteFile(path, text);
    }
    CopyTextToClipboard(text);
    HWND parent = win ? win->hwndFrame : nullptr;
    MessageBoxW(parent, CWStrTemp(text ? text : StrL("(empty)")), CWStrTemp(_TRA("开发信息")),
                MB_OK | MB_ICONINFORMATION);
    logf("ShowDevInfoDialog: done\n");
}

bool ProbeCdpPortAlive(int port, int* elapsedMsOut, Str* errorOut) {
    CdpProbe p = ProbeCdp(port);
    if (elapsedMsOut) {
        *elapsedMsOut = p.elapsedMs;
    }
    if (errorOut) {
        *errorOut = p.httpOk ? Str{} : str::Dup(p.error ? p.error : StrL("failed"));
    }
    bool ok = p.httpOk;
    FreeCdpProbe(&p);
    return ok;
}
