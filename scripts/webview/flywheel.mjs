#!/usr/bin/env node
/**
 * AI/Cursor closed-loop flywheel for SumatraPDF WebPanel + NotebookLM.
 *
 *   node flywheel.mjs [--pdf PATH] [--bookId N] [--skip-build] [--reuse]
 *                     [--q QUESTION] [--no-chat]
 *
 * Steps:
 *   1. (optional) bun cmd/build.ts -debug
 *   2. stop prior out/dbg64 SumatraPDF
 *   3. launch exe -for-testing -log with PDF (or reuse live CDP)
 *   4. wait for CDP 9224 (fallback 9223)
 *   5. drain/add NotebookLM job (same as library right-click "Add to NotebookLM")
 *   6. verify chat reply
 *   7. print JSON report
 */
import fs from "node:fs";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { ensureJobDirs, bridgePaths, readJson, listPendingJobs, writeJson } from "./lib/bridge.mjs";
import { probeCdpPorts } from "./lib/notebooklm.mjs";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(__dirname, "..", "..");
const EXE = path.join(REPO, "out", "dbg64", "SumatraPDF.exe");

function arg(name, fallback = null) {
  const i = process.argv.indexOf(`--${name}`);
  if (i >= 0 && process.argv[i + 1]) return process.argv[i + 1];
  return fallback;
}
function has(flag) {
  return process.argv.includes(`--${flag}`);
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

function runNode(script, args, opts = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [script, ...args], {
      cwd: __dirname,
      stdio: opts.silent ? "pipe" : "inherit",
      windowsHide: true,
      env: process.env,
    });
    let out = "";
    if (opts.silent) {
      child.stdout.on("data", (d) => (out += d));
      child.stderr.on("data", (d) => (out += d));
    }
    child.on("exit", (code) => {
      if (code === 0) resolve(out);
      else reject(new Error(`${path.basename(script)} exit ${code}\n${out}`));
    });
  });
}

function killSumatraDbg() {
  spawnSync(
    "powershell.exe",
    [
      "-NoProfile",
      "-Command",
      `Get-Process SumatraPDF -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*\\out\\dbg64\\*' } | Stop-Process -Force`,
    ],
    { stdio: "inherit" },
  );
}

async function waitCdp(timeoutMs = 90000) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    const p = await probeCdpPorts(9224);
    if (p.best) return p;
    await sleep(1500);
  }
  throw new Error("CDP not ready");
}

async function main() {
  const report = {
    startedAt: new Date().toISOString(),
    steps: {},
  };
  ensureJobDirs();

  const pdf = arg("pdf");
  if (!pdf) {
    throw new Error("missing --pdf PATH (required)");
  }
  const bookId = arg("bookId", "0");
  const question = arg("q", "用一句话概括这本书的主题");
  const reuse = has("reuse");
  const skipBuild = has("skip-build") || reuse;
  const noChat = has("no-chat");

  if (!fs.existsSync(pdf)) {
    throw new Error(`pdf missing: ${pdf}`);
  }

  if (!skipBuild) {
    console.error("[flywheel] building dbg64…");
    const b = spawnSync("bun", ["cmd/build.ts", "-debug"], {
      cwd: REPO,
      stdio: "inherit",
      shell: true,
    });
    report.steps.build = { ok: b.status === 0, code: b.status };
    if (b.status !== 0) throw new Error("build failed");
  } else {
    report.steps.build = { skipped: true };
  }

  if (!reuse) {
    if (!fs.existsSync(EXE)) throw new Error(`missing exe: ${EXE}`);
    console.error("[flywheel] restarting SumatraPDF…");
    killSumatraDbg();
    await sleep(800);
    const child = spawn(
      EXE,
      ["-for-testing", "-log", "-window-pos", "1280x800@40x40", pdf],
      {
        cwd: path.dirname(EXE),
        detached: true,
        stdio: "ignore",
        windowsHide: false,
      },
    );
    child.unref();
    report.steps.launch = { ok: true, pid: child.pid, exe: EXE, pdf };
  } else {
    report.steps.launch = { reused: true };
  }

  console.error("[flywheel] waiting CDP…");
  const cdp = await waitCdp();
  report.steps.cdp = cdp.best;
  writeJson(bridgePaths().bridgeCanonical, {
    ...(cdp.bridge || {}),
    cdpPort: cdp.best.port,
    cdpEndpoint: `http://127.0.0.1:${cdp.best.port}`,
    pdfPath: pdf,
    fileName: path.basename(pdf),
  });

  // Give WebView a moment after first paint
  await sleep(2500);

  console.error("[flywheel] drain pending + add…");
  const pendingBefore = listPendingJobs();
  try {
    await runNode(path.join(__dirname, "cli.mjs"), ["drain"]);
    report.steps.drain = { ok: true, pendingBefore: pendingBefore.length };
  } catch (e) {
    report.steps.drain = { ok: false, error: String(e) };
  }

  // Always run a fresh add for the target PDF so the loop is deterministic
  try {
    await runNode(path.join(__dirname, "cli.mjs"), [
      "add",
      "--pdf",
      pdf,
      "--bookId",
      String(bookId),
    ]);
    report.steps.add = { ok: true, pdf, bookId: Number(bookId) };
  } catch (e) {
    report.steps.add = { ok: false, error: String(e) };
    report.ok = false;
    console.log(JSON.stringify(report, null, 2));
    process.exit(1);
  }

  // Read latest done result
  const doneDir = bridgePaths().jobsDone;
  const doneFiles = fs
    .readdirSync(doneDir)
    .filter((n) => n.endsWith(".json"))
    .map((n) => ({ n, t: fs.statSync(path.join(doneDir, n)).mtimeMs }))
    .sort((a, b) => b.t - a.t);
  if (doneFiles[0]) {
    report.steps.addResult = readJson(path.join(doneDir, doneFiles[0].n));
  }

  if (!noChat) {
    console.error("[flywheel] chat probe…");
    try {
      const out = await runNode(
        path.join(__dirname, "notebooklm-chat.mjs"),
        ["--q", question],
        { silent: true },
      );
      const jsonStart = out.indexOf("{");
      report.steps.chat = jsonStart >= 0 ? JSON.parse(out.slice(jsonStart)) : { raw: out };
    } catch (e) {
      report.steps.chat = { ok: false, error: String(e) };
    }
  }

  report.finishedAt = new Date().toISOString();
  report.ok =
    !!report.steps.add?.ok &&
    (noChat || report.steps.chat?.ok === true);
  console.log(JSON.stringify(report, null, 2));
  process.exit(report.ok ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
