# SumatraPDF ↔ WebView script bridge (AI / Cursor closed loop)

Drive GUI-equivalent actions (library right-click **Add to NotebookLM**, chat, …)
without a human at the mouse: compile → launch exe → CDP/job queue → verify.

## Architecture

```
Cursor / node cli.mjs flywheel
        |
        v
 bun build → out/dbg64/SumatraPDF.exe  --CDP 9224-->  WebView2
        |                                              ^
        |  -dbg-control TestWebPanel                   | Playwright connectOverCDP
        |  jobs/*.json + web-bridge.json               |
        v                                              |
 scripts/webview/*.mjs  -------------------------------+
        |
        v
 books.notebooklm (SQLite JSON)  <--- jobs/done/*.json polled by exe
```

## One command flywheel

```bash
cd scripts/webview
npm install
# connectOverCDP uses the live Sumatra WebView2 — full Chromium download is optional

# Full loop: build, restart exe, add PDF, ask NotebookLM
node cli.mjs flywheel --pdf "C:\path\to\book.pdf" --bookId 37

# Reuse already-running Sumatra (faster iteration)
node cli.mjs flywheel --reuse --pdf "C:\path\to\book.pdf" --bookId 37 --skip-build
```

## CLI surface (no GUI clicks)

| Command | Meaning |
|---------|---------|
| `node cli.mjs status` | bridge JSON, CDP ports, pending/done jobs |
| `node cli.mjs cdp` | probe 9224/9223 |
| `node cli.mjs drain` | process all `jobs/pending` |
| `node cli.mjs add --pdf PATH --bookId N` | same as context-menu Add to NotebookLM |
| `node cli.mjs select --title NAME` | select source after reopen |
| `node cli.mjs chat --q "..."` | send NotebookLM chat + wait for reply |
| `node cli.mjs test-add --pdf PATH` | add against whatever CDP is live |
| `node cli.mjs flywheel …` | build → launch → add → chat report |

## C++ dbg-control (`TestWebPanel` = 73)

With `SumatraPDF.exe -for-testing -dbg-control <pipe>`:

| action | args | effect |
|--------|------|--------|
| `show` | | open WebPanel on NotebookLM (no white Navigate if tab exists) |
| `hide` | | close panel |
| `status` | | visible/tabs/cdp/url |
| `add` | pdfPath, bookId?, title? | queue + spawn `notebooklm-add.mjs` |
| `poll` | | apply `jobs/done` → `books.notebooklm` |
| `notebooklm` | path? | dump stored JSON for book |

## NotebookLM rules

- Notebooks: `SumatraPDF1`, `SumatraPDF2`, … (count / prefix from `config.json`)
- Default: **50 sources per notebook**, max 40 notebooks — edit via 图书馆右键 → **脚本管理**
- Add fills the lowest notebook with free slots, then rolls to the next
- On success, job result updates `books.notebooklm` via host poll of `jobs/done`

## How it works (JS + Playwright, not Python)

```
SumatraPDF.exe (C++)
  └─ WebView2  --remote-debugging-port=9224
  └─ writes jobs/pending/*.json
  └─ LaunchProcessHidden → node.exe notebooklm-*.mjs
         └─ playwright.chromium.connectOverCDP("http://127.0.0.1:9224")
         └─ UI-automate NotebookLM (click 来源 / upload / select / 对话)
         └─ writes jobs/done/*.json
  └─ C++ polls done/ → updates SQLite books.notebooklm
```

Scripts live on disk under `scripts/webview/` (never baked into the exe). Edit + Save in **脚本管理** takes effect on the next run.

## 脚本管理

Library context menu → **脚本管理**:
- Category tree from `manifest.json`
- VS Code–like editor = external `manager/editor.html` (WebView2; not linked into exe)
- Tunables in `config.json` (`sourcesPerNotebook`, …)
- Save / Format / Run / Open folder

## Paths

| Location | Role |
|----------|------|
| `<repo>/scripts/webview/` | Source of truth in the git checkout |
| `%OneDrive%\SumatraPDF\scripts\webview\` (or portable appdata) | Synced on launch from the repo; run `npm ci` here once |
| `%OneDrive%\SumatraPDF\WebPanel\` | **Synced** small state: tabs, pdf-map, `bridge/web-bridge.json` |
| `%LOCALAPPDATA%\SumatraPDF\WebPanel\` | **Local** heavy state: WebView2 profiles, jobs, favicon cache |
| `SUMATRA_SCRIPTS` | Optional override directory (must contain `package.json`) |

CDP: **9224** = Browser-AIChat (NotebookLM), **9225** = Browser-Library (center Web).
