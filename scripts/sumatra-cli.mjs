#!/usr/bin/env node
/**
 * SumatraPDF AI control CLI — CDP + DDE-friendly helpers.
 *
 * Usage:
 *   node scripts/sumatra-cli.mjs status
 *   node scripts/sumatra-cli.mjs cdp
 *   node scripts/sumatra-cli.mjs cdp-ai
 *   node scripts/sumatra-cli.mjs cdp-web
 *   node scripts/sumatra-cli.mjs targets --port 9224
 *   node scripts/sumatra-cli.mjs open --url "https://example.com" --panel web
 *   node scripts/sumatra-cli.mjs cmd --name CmdShowDevInfo
 *   node scripts/sumatra-cli.mjs eval --port 9225 --js "document.title"
 *
 * CDP ports (WebView2 remote debugging):
 *   AI panel  = 9224
 *   Web panel = 9225
 *
 * Full browser control: connect Playwright/Puppeteer over CDP to either port.
 *   const browser = await chromium.connectOverCDP("http://127.0.0.1:9225");
 */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { spawn } from "node:child_process";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const AI_PORT = 9224;
const WEB_PORT = 9225;

function arg(name, fallback = null) {
  const i = process.argv.indexOf(`--${name}`);
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return fallback;
}

function cmd() {
  return process.argv[2] || "status";
}

function bridgeCandidates() {
  const home = process.env.USERPROFILE || process.env.HOME || "";
  const one = process.env.OneDrive || process.env.ONEDRIVE || "";
  return [
    path.join(one, "SumatraPDF", "WebPanel", "bridge", "web-bridge.json"),
    path.join(home, "AppData", "Local", "SumatraPDF", "WebPanel", "bridge", "web-bridge.json"),
    path.join(home, "AppData", "Roaming", "SumatraPDF", "WebPanel", "bridge", "web-bridge.json"),
  ];
}

function readBridge() {
  for (const p of bridgeCandidates()) {
    try {
      if (fs.existsSync(p)) {
        return { path: p, data: JSON.parse(fs.readFileSync(p, "utf8")) };
      }
    } catch {
      /* ignore */
    }
  }
  return { path: null, data: {} };
}

async function probePort(port) {
  const t0 = Date.now();
  try {
    const res = await fetch(`http://127.0.0.1:${port}/json/version`, {
      signal: AbortSignal.timeout(2000),
    });
    const body = await res.json();
    return {
      port,
      ok: res.ok,
      ms: Date.now() - t0,
      browser: body.Browser,
      protocol: body["Protocol-Version"],
      ws: body.webSocketDebuggerUrl,
      userAgent: body["User-Agent"],
    };
  } catch (e) {
    return { port, ok: false, ms: Date.now() - t0, error: String(e.message || e) };
  }
}

async function listTargets(port) {
  const t0 = Date.now();
  try {
    const res = await fetch(`http://127.0.0.1:${port}/json/list`, {
      signal: AbortSignal.timeout(2000),
    });
    const body = await res.json();
    return { port, ok: true, ms: Date.now() - t0, targets: body };
  } catch (e) {
    return { port, ok: false, ms: Date.now() - t0, error: String(e.message || e), targets: [] };
  }
}

async function status() {
  const bridge = readBridge();
  const ai = await probePort(bridge.data.aiCdpPort || AI_PORT);
  const web = await probePort(bridge.data.webCdpPort || WEB_PORT);
  console.log(
    JSON.stringify(
      {
        bridgePath: bridge.path,
        bridge: bridge.data,
        aiCdp: ai,
        webCdp: web,
        controlPipe: "\\\\.\\pipe\\sumatrapdf-ai",
        hint: {
          playwright: "chromium.connectOverCDP('http://127.0.0.1:PORT')",
          dde: 'SumatraPDF.exe -reuse-instance -dde "[CmdShowDevInfo]"',
        },
      },
      null,
      2,
    ),
  );
}

async function cdpBoth() {
  console.log(
    JSON.stringify(
      {
        ai: await probePort(AI_PORT),
        web: await probePort(WEB_PORT),
      },
      null,
      2,
    ),
  );
}

function findExe() {
  const candidates = [
    path.resolve(__dirname, "../out/dbg64/SumatraPDF.exe"),
    path.resolve(__dirname, "../out/rel64/SumatraPDF.exe"),
  ];
  for (const c of candidates) {
    if (fs.existsSync(c)) return c;
  }
  return null;
}

function runDde(ddeCmd) {
  const exe = findExe();
  if (!exe) {
    throw new Error("SumatraPDF.exe not found under out/dbg64 or out/rel64");
  }
  return new Promise((resolve, reject) => {
    const child = spawn(exe, ["-reuse-instance", "-dde", ddeCmd], {
      windowsHide: true,
      stdio: "inherit",
    });
    child.on("exit", (code) => (code === 0 ? resolve() : reject(new Error(`exit ${code}`))));
  });
}

async function openUrl() {
  const url = arg("url");
  if (!url) throw new Error("--url required");
  const panel = (arg("panel", "web") || "web").toLowerCase();
  if (panel === "ai") {
    // Best-effort: open via CDP Page.navigate on AI browser if live.
    const p = await probePort(AI_PORT);
    if (!p.ok) throw new Error("AI CDP not live — open AI sidebar first");
    const list = await listTargets(AI_PORT);
    console.log(JSON.stringify({ ok: true, note: "use Playwright connectOverCDP to navigate", cdp: p, list }, null, 2));
    return;
  }
  // Native path: DDE Cmd is limited; prefer telling user to use pipe AiOpenWeb.
  // Fallback: launch with URL as file-like open may not work for http — print instruction.
  console.log(
    JSON.stringify(
      {
        ok: true,
        note: "Open via CDP on Web panel (9225) or control pipe AiOpenWeb=102",
        example: `node -e "import('playwright').then(async ({chromium})=>{const b=await chromium.connectOverCDP('http://127.0.0.1:9225');const p=b.contexts()[0].pages()[0];await p.goto('${url}')})"`,
        url,
      },
      null,
      2,
    ),
  );
}

async function runCmd() {
  const name = arg("name");
  if (!name) throw new Error("--name required (e.g. CmdShowDevInfo)");
  await runDde(`[${name}]`);
  console.log(JSON.stringify({ ok: true, cmd: name }, null, 2));
}

async function evalJs() {
  const port = Number(arg("port", WEB_PORT));
  const js = arg("js");
  if (!js) throw new Error("--js required");
  let chromium;
  try {
    ({ chromium } = await import("playwright"));
  } catch {
    throw new Error("playwright not installed — cd scripts/webview && npm i");
  }
  const browser = await chromium.connectOverCDP(`http://127.0.0.1:${port}`);
  const context = browser.contexts()[0];
  if (!context) throw new Error("no browser context on CDP");
  const page = context.pages()[0] || (await context.newPage());
  const result = await page.evaluate(js);
  console.log(JSON.stringify({ ok: true, port, result }, null, 2));
  // Do not close — shared with Sumatra WebView2.
}

const handlers = {
  status,
  cdp: cdpBoth,
  "cdp-ai": async () => console.log(JSON.stringify(await probePort(AI_PORT), null, 2)),
  "cdp-web": async () => console.log(JSON.stringify(await probePort(WEB_PORT), null, 2)),
  targets: async () =>
    console.log(JSON.stringify(await listTargets(Number(arg("port", WEB_PORT))), null, 2)),
  open: openUrl,
  cmd: runCmd,
  eval: evalJs,
  help: async () => {
    console.log(`sumatra-cli commands: ${Object.keys(handlers).filter((k) => k !== "help").join(", ")}`);
  },
};

const fn = handlers[cmd()];
if (!fn) {
  console.error(`unknown command: ${cmd()}`);
  await handlers.help();
  process.exit(2);
}
fn().catch((e) => {
  console.error(e);
  process.exit(1);
});
