# SumatraPDF browsers / agents — extensions

Unified plugin root for WebView2 (`Browser-AIChat`, `Browser-Library`) and future agents.

## Layout

- `installed/<id>/plugin.json` — **host** plugins (native / Playwright actions)
- `installed/<id>/manifest.json` — unpacked Chromium / WebView2 extensions (`AddBrowserExtension`)
- `overlays/<id>.json` — **overlay descriptors only** (tiny, git-tracked). Payload is **not** in git.

### Overlay mode (Chrome → WebView2)

Large Chrome extensions (e.g. Trancy) stay in Chrome’s user data. On launch Sumatra:

1. Reads `overlays/<id>.json` (`chromeId`, `optionsPage`, `browser`)
2. Copies that Chrome unpacked folder → `%LOCALAPPDATA%\SumatraPDF\extensions\overlay\<id>\`
3. Strips `_metadata` (WebView2 rejects it)
4. Center browser (`Browser-Library`) loads via `AddBrowserExtension`

Do **not** commit `extensions/overlay/` or copied CRX trees.

Host `plugin.json` / overlay `browser` field:

- `Browser-AIChat` — only AI panel puzzle menu (CDP 9224)
- `Browser-Library` — only center Web puzzle menu (CDP 9225)

Built-in NotebookLM host plugins are **Browser-AIChat only** and operate on the **center pane** (PDF or webpage).

Runtime: `%OneDrive%\SumatraPDF\extensions\` (host plugins + overlay JSON) and `%LOCALAPPDATA%\SumatraPDF\extensions\overlay\` (Chrome payloads).

Repo: https://github.com/tingaidehua/sumatrapdf-browsers-agents
