import fs from "node:fs";
import path from "node:path";
import os from "node:os";

export function oneDriveSumatraDir() {
  const od = process.env.OneDrive || path.join(os.homedir(), "OneDrive");
  return path.join(od, "SumatraPDF");
}

export function localSumatraDir() {
  const la = process.env.LOCALAPPDATA || path.join(os.homedir(), "AppData", "Local");
  return path.join(la, "SumatraPDF");
}

/** Prefer structured layout; fall back to legacy flat files during migration. */
function prefer(newPath, legacyPath) {
  if (fs.existsSync(newPath)) return newPath;
  if (legacyPath && fs.existsSync(legacyPath)) return legacyPath;
  return newPath;
}

/** Prefer machine-local heavy paths; fall back to synced OneDrive copies. */
function preferLocal(localPath, syncPath, legacySyncPath) {
  if (fs.existsSync(localPath)) return localPath;
  if (syncPath && fs.existsSync(syncPath)) return syncPath;
  if (legacySyncPath && fs.existsSync(legacySyncPath)) return legacySyncPath;
  return localPath;
}

export function bridgePaths() {
  const root = oneDriveSumatraDir();
  const webPanel = path.join(root, "WebPanel");
  const localWebPanel = path.join(localSumatraDir(), "WebPanel");
  const bridgeNew = path.join(webPanel, "bridge", "web-bridge.json");
  const bridgeLegacy = path.join(webPanel, "web-bridge.json");
  return {
    root,
    webPanel,
    localWebPanel,
    // Small state stays synced (OneDrive / portable appdata).
    bridge: prefer(bridgeNew, bridgeLegacy),
    bridgeCanonical: bridgeNew,
    tabsIndex: prefer(path.join(webPanel, "tabs", "index.json"), path.join(webPanel, "tabs.json")),
    pdfTabMap: prefer(path.join(webPanel, "tabs", "pdf-map.json"), path.join(webPanel, "pdf-tabs.json")),
    // Heavy state: LOCALAPPDATA first, then legacy OneDrive trees.
    jobsPending: preferLocal(
      path.join(localWebPanel, "jobs", "pending"),
      path.join(webPanel, "jobs", "pending")
    ),
    jobsDone: preferLocal(path.join(localWebPanel, "jobs", "done"), path.join(webPanel, "jobs", "done")),
    jobsFailed: preferLocal(
      path.join(localWebPanel, "jobs", "failed"),
      path.join(webPanel, "jobs", "failed")
    ),
    profileDir: preferLocal(
      path.join(localWebPanel, "profile", "Browser-AIChat"),
      path.join(webPanel, "profile", "Browser-AIChat"),
      path.join(webPanel, "profile", "WebView2")
    ),
    profileLibraryDir: preferLocal(
      path.join(localWebPanel, "profile", "Browser-Library"),
      path.join(webPanel, "profile", "Browser-Library"),
      path.join(webPanel, "profile", "WebView2-Browser")
    ),
    faviconsDir: preferLocal(
      path.join(localWebPanel, "cache", "favicons"),
      path.join(webPanel, "cache", "favicons"),
      path.join(webPanel, "favicons")
    ),
    bridgeLog: preferLocal(
      path.join(localWebPanel, "bridge", "bridge.log"),
      path.join(webPanel, "bridge", "bridge.log"),
      path.join(webPanel, "bridge.log")
    ),
  };
}

export function readJson(file, fallback = null) {
  try {
    return JSON.parse(fs.readFileSync(file, "utf8"));
  } catch {
    return fallback;
  }
}

export function writeJson(file, obj) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, JSON.stringify(obj, null, 2), "utf8");
}

export function ensureJobDirs() {
  const p = bridgePaths();
  // Always create canonical LOCAL job dirs so new writes leave OneDrive.
  const localJobs = [
    path.join(p.localWebPanel, "jobs", "pending"),
    path.join(p.localWebPanel, "jobs", "done"),
    path.join(p.localWebPanel, "jobs", "failed"),
  ];
  for (const d of localJobs) {
    fs.mkdirSync(d, { recursive: true });
  }
  for (const d of [p.jobsPending, p.jobsDone, p.jobsFailed]) {
    fs.mkdirSync(d, { recursive: true });
  }
  // Keep bridge writes on the synced canonical path.
  fs.mkdirSync(path.dirname(p.bridgeCanonical), { recursive: true });
  return p;
}

export function listPendingJobs() {
  const p = ensureJobDirs();
  return fs
    .readdirSync(p.jobsPending)
    .filter((n) => n.endsWith(".json"))
    .map((n) => path.join(p.jobsPending, n))
    .sort();
}

export function finishJob(jobPath, ok, result) {
  const p = bridgePaths();
  const base = path.basename(jobPath);
  const dest = path.join(ok ? p.jobsDone : p.jobsFailed, base);
  writeJson(dest, { ...readJson(jobPath, {}), result, finishedAt: Date.now(), ok });
  try {
    fs.unlinkSync(jobPath);
  } catch {
    /* ignore */
  }
  return dest;
}
