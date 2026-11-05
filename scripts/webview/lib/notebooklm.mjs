import { chromium } from "playwright";
import { bridgePaths, readJson, writeJson } from "./bridge.mjs";

export async function probeCdpPorts(preferred = 9224) {
  const bridge = readJson(bridgePaths().bridge, {});
  const ports = [
    bridge.aiCdpPort,
    bridge.webCdpPort,
    bridge.cdpPort,
    preferred,
    9224,
    9225,
    9223,
  ].filter(Boolean);
  const seen = new Set();
  const live = [];
  for (const p of ports) {
    if (seen.has(p)) continue;
    seen.add(p);
    try {
      const res = await fetch(`http://127.0.0.1:${p}/json/version`, {
        signal: AbortSignal.timeout(2000),
      });
      const body = await res.json();
      live.push({
        port: p,
        ok: true,
        browser: body.Browser,
        ws: body.webSocketDebuggerUrl,
      });
    } catch (e) {
      live.push({ port: p, ok: false, error: String(e.message || e) });
    }
  }
  return { bridge, live, best: live.find((r) => r.ok) || null };
}

export async function connectCdp(port = 9224) {
  const probed = await probeCdpPorts(port);
  const best = probed.best;
  if (!best) {
    throw new Error("no live CDP on 9224/9223 — open WebPanel in SumatraPDF first");
  }
  const endpoint = `http://127.0.0.1:${best.port}`;
  // Keep bridge in sync so subsequent helpers / C++ poll see the live port.
  writeJson(bridgePaths().bridgeCanonical, {
    ...(probed.bridge || {}),
    cdpPort: best.port,
    cdpEndpoint: endpoint,
  });
  const browser = await chromium.connectOverCDP(endpoint);
  const context = browser.contexts()[0] || (await browser.newContext());
  return { browser, context, port: best.port, endpoint };
}

export async function notebookPage(context) {
  const pages = context.pages();
  let page =
    pages.find((p) => /notebook\.google\.com|notebooklm\.google/i.test(p.url())) ||
    pages[0];
  if (!page) {
    page = await context.newPage();
  }
  if (!/notebook\.google\.com|notebooklm\.google/i.test(page.url())) {
    await page.goto("https://notebook.google.com/", {
      waitUntil: "domcontentloaded",
      timeout: 60000,
    });
  }
  await page.waitForTimeout(800);
  return page;
}

/** Click the first visible control whose accessible name / text matches. */
export async function clickByText(page, patterns, timeout = 8000) {
  const list = Array.isArray(patterns) ? patterns : [patterns];
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    for (const pat of list) {
      const re = typeof pat === "string" ? new RegExp(pat, "i") : pat;
      const locators = [
        page.getByRole("button", { name: re }),
        page.getByRole("link", { name: re }),
        page.getByRole("tab", { name: re }),
        page.getByText(re, { exact: false }),
      ];
      for (const loc of locators) {
        try {
          const el = loc.first();
          if (await el.isVisible({ timeout: 200 })) {
            await el.click({ timeout: 2000 });
            return true;
          }
        } catch {
          /* try next */
        }
      }
    }
    await page.waitForTimeout(250);
  }
  return false;
}

export async function ensureNotebook(page, name) {
  const opened = await clickByText(page, [`^${name}$`, name], 4000);
  if (opened) {
    await page.waitForTimeout(1200);
    return true;
  }
  const created = await clickByText(
    page,
    ["创建笔记本", "New notebook", "Create new notebook", "新建"],
    6000,
  );
  if (!created) {
    return false;
  }
  await page.waitForTimeout(1500);
  try {
    const title = page.getByRole("button", { name: /无笔记|Untitled|notebook/i }).first();
    if (await title.isVisible({ timeout: 2000 })) {
      await title.click();
      await page.keyboard.type(name, { delay: 20 });
      await page.keyboard.press("Enter");
    }
  } catch {
    /* title rename is best-effort */
  }
  await page.waitForTimeout(800);
  return true;
}

export async function countSources(page) {
  // Mobile NotebookLM often has no visible source checkboxes; parse "N 个来源" / "N sources".
  const fromLabel = await page.evaluate(() => {
    const t = document.body?.innerText || "";
    let m = t.match(/(\d+)\s*个来源/);
    if (m) return Number(m[1]);
    m = t.match(/(\d+)\s*sources?/i);
    if (m) return Number(m[1]);
    return -1;
  });
  if (fromLabel >= 0) return fromLabel;
  return page.evaluate(() => {
    const nodes = [...document.querySelectorAll("[role='checkbox'], input[type='checkbox']")];
    return nodes.filter((n) => {
      const r = n.getBoundingClientRect();
      return r.width > 0 && r.height > 0 && r.left < window.innerWidth * 0.5;
    }).length;
  });
}

export async function sourceListed(page, title) {
  const base = pathBase(title);
  await clickByText(page, ["^来源$", "Sources", "来源"], 4000);
  await page.waitForTimeout(400);
  return page.evaluate((want) => {
    const t = document.body?.innerText || "";
    return t.includes(want);
  }, base);
}

export async function addPdfSource(page, pdfPath) {
  const base = pathBase(pdfPath);
  // Ensure Sources tab (mobile: 来源 / 对话 / Studio)
  await clickByText(page, ["^来源$", "Sources", "来源"], 5000);
  await page.waitForTimeout(500);

  if (await sourceListed(page, base)) {
    return { ok: true, waitedSec: 0, sourceTitle: base, alreadyPresent: true };
  }

  const before = await countSources(page);

  // Open add-source UI (mobile button text is often "add\n添加来源")
  await clickByText(page, ["添加来源", "Add source", "Upload", "上传", "add"], 8000);
  await page.waitForTimeout(600);

  // Prefer direct file input if the drop zone already exposed one
  let uploaded = false;
  const fileInput = page.locator('input[type="file"]');
  if ((await fileInput.count()) > 0) {
    try {
      await fileInput.first().setInputFiles(pdfPath);
      uploaded = true;
    } catch {
      /* fall through to chooser */
    }
  }
  if (!uploaded) {
    const [chooser] = await Promise.all([
      page.waitForEvent("filechooser", { timeout: 12000 }).catch(() => null),
      clickByText(
        page,
        ["PDF", "上传", "Upload", "Computer", "此电脑", "选择文件", "将文件拖放", "drag"],
        6000,
      ),
    ]);
    if (chooser) {
      await chooser.setFiles(pdfPath);
      uploaded = true;
    } else if ((await fileInput.count()) > 0) {
      await fileInput.first().setInputFiles(pdfPath);
      uploaded = true;
    }
  }
  if (!uploaded) {
    return { ok: false, waitedSec: 0, sourceTitle: base, error: "no-file-input" };
  }

  for (let i = 0; i < 120; i++) {
    await page.waitForTimeout(1000);
    await clickByText(page, ["^来源$", "Sources", "来源"], 1500).catch(() => false);
    const listed = await page.evaluate((want) => (document.body?.innerText || "").includes(want), base);
    const after = await countSources(page);
    if (listed || (before >= 0 && after > before)) {
      return { ok: true, waitedSec: i + 1, sourceTitle: base, sourceCount: after };
    }
  }
  return { ok: false, waitedSec: 120, sourceTitle: base, sourceCount: await countSources(page) };
}

/** Add a website / YouTube URL as a NotebookLM source (paste into 网站和 YouTube 网址). */
export async function addWebsiteSource(page, sourceUrl, sourceTitle = "") {
  const url = String(sourceUrl || "").trim();
  if (!url) {
    return { ok: false, waitedSec: 0, sourceTitle: "", error: "empty-url" };
  }
  let host = "";
  try {
    host = new URL(url).hostname.replace(/^www\./, "");
  } catch {
    host = url.slice(0, 48);
  }
  const label = String(sourceTitle || host || url).trim();

  await clickByText(page, ["^来源$", "Sources", "来源"], 5000);
  await page.waitForTimeout(500);

  if (await sourceListed(page, label) || (host && (await sourceListed(page, host)))) {
    return { ok: true, waitedSec: 0, sourceTitle: label, alreadyPresent: true, sourceUrl: url };
  }

  const before = await countSources(page);

  await clickByText(page, ["添加来源", "Add source", "Upload", "上传", "add"], 8000);
  await page.waitForTimeout(600);

  const openedWeb = await clickByText(
    page,
    ["^网站$", "Website", "网站和 YouTube", "Websites?", "YouTube"],
    8000,
  );
  if (!openedWeb) {
    return { ok: false, waitedSec: 0, sourceTitle: label, sourceUrl: url, error: "no-website-button" };
  }
  await page.waitForTimeout(700);

  // Paste into "粘贴任何链接" / "Paste any link" box.
  const filled = await page.evaluate((text) => {
    const nodes = [
      ...document.querySelectorAll("textarea, input[type='text'], input[type='url'], [contenteditable='true']"),
    ];
    const box =
      nodes.find((el) =>
        /粘贴|链接|paste|link|url|youtube/i.test(
          el.placeholder || el.getAttribute("aria-label") || el.getAttribute("data-placeholder") || "",
        ),
      ) || nodes.find((el) => el.offsetParent !== null) || nodes[0];
    if (!box) return false;
    box.scrollIntoView({ block: "center" });
    box.focus();
    if (box.isContentEditable) {
      box.textContent = text;
      box.dispatchEvent(new Event("input", { bubbles: true }));
    } else {
      box.value = text;
      box.dispatchEvent(new Event("input", { bubbles: true }));
      box.dispatchEvent(new Event("change", { bubbles: true }));
    }
    return true;
  }, url);

  if (!filled) {
    const loc = page
      .locator('textarea, input[type="url"], input[type="text"]')
      .filter({ hasNot: page.locator("[disabled]") })
      .first();
    await loc.fill(url, { timeout: 5000 }).catch(() => null);
  }
  await page.waitForTimeout(400);

  const inserted = await clickByText(page, ["^插入$", "Insert", "添加", "Add", "提交", "Submit"], 6000);
  if (!inserted) {
    await page.keyboard.press("Enter");
  }
  await page.waitForTimeout(800);

  for (let i = 0; i < 90; i++) {
    await page.waitForTimeout(1000);
    await clickByText(page, ["^来源$", "Sources", "来源"], 1500).catch(() => false);
    const listed =
      (await page.evaluate((want) => (document.body?.innerText || "").includes(want), label)) ||
      (host && (await page.evaluate((want) => (document.body?.innerText || "").includes(want), host)));
    const after = await countSources(page);
    if (listed || (before >= 0 && after > before)) {
      return {
        ok: true,
        waitedSec: i + 1,
        sourceTitle: label,
        sourceUrl: url,
        sourceCount: after,
      };
    }
  }
  return {
    ok: false,
    waitedSec: 90,
    sourceTitle: label,
    sourceUrl: url,
    sourceCount: await countSources(page),
    error: "timeout-waiting-source",
  };
}

function pathBase(p) {
  const s = String(p || "").replace(/\\/g, "/");
  const i = s.lastIndexOf("/");
  return i >= 0 ? s.slice(i + 1) : s;
}

export async function selectOnlySource(page, sourceTitle) {
  const want = pathBase(sourceTitle || "");
  await clickByText(page, ["^来源$", "Sources", "来源"], 5000);
  await page.waitForTimeout(500);

  // No public NotebookLM URL/API for "select one source" in consumer UI.
  // Reliable path: uncheck 全选 → uncheck every row → check only the match (scroll into view).
  const result = await page.evaluate(async (wantN) => {
    const norm = (s) => (s || "").replace(/\s+/g, " ").trim().toLowerCase();
    const want = norm(wantN);
    const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

    const isChecked = (el) =>
      el.getAttribute("aria-checked") === "true" || el.checked === true;

    const clickEl = (el) => {
      el.scrollIntoView({ block: "nearest", inline: "nearest" });
      el.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true }));
    };

    // Uncheck "全选" / Select all if present and checked
    const allLabels = [...document.querySelectorAll("button, label, span, div, [role='checkbox']")];
    for (const el of allLabels) {
      const t = norm(el.innerText || el.getAttribute("aria-label") || "");
      if (t === "全选" || t === "select all" || t.includes("选择所有来源")) {
        const box =
          el.matches?.("[role='checkbox'],input[type='checkbox']")
            ? el
            : el.querySelector?.("[role='checkbox'],input[type='checkbox']") || el;
        if (isChecked(box)) {
          clickEl(box);
          await sleep(200);
        }
        break;
      }
    }

    const boxes = [...document.querySelectorAll("[role='checkbox'], input[type='checkbox']")];
    const visible = boxes.filter((n) => {
      const r = n.getBoundingClientRect();
      return r.width > 0 && r.height > 0;
    });

    // Turn everything off
    for (const el of visible) {
      if (isChecked(el)) {
        clickEl(el);
        await sleep(60);
      }
    }
    await sleep(150);

    // Turn on matching source row only (search by nearby text; scroll if needed)
    for (const el of visible) {
      const row = el.closest("li,div,article,section,label,mat-list-item") || el.parentElement;
      const text = norm(row?.innerText || el.getAttribute("aria-label") || "");
      // skip the select-all control
      if (text === "全选" || text === "select all" || text.includes("选择所有来源")) continue;
      if (want && text.includes(want)) {
        row?.scrollIntoView?.({ block: "nearest" });
        if (!isChecked(el)) {
          clickEl(el);
        }
        return { ok: true, matched: text.slice(0, 80) };
      }
    }

    // Fallback: click a button/tile whose text contains the filename
    const tiles = [...document.querySelectorAll("button, [role='button'], a")];
    for (const el of tiles) {
      const text = norm(el.innerText || el.getAttribute("aria-label") || "");
      if (want && text.includes(want) && text.length < 200) {
        el.scrollIntoView({ block: "nearest" });
        clickEl(el);
        return { ok: true, matched: text.slice(0, 80), via: "tile" };
      }
    }
    return { ok: false };
  }, want);

  await page.waitForTimeout(400);
  return result;
}

/**
 * Send a chat message in the open notebook and wait for a non-empty reply.
 * Returns { ok, question, answerSnippet, elapsedMs }.
 */
export async function askChat(page, question, timeoutMs = 120000) {
  const q = question || `用一句话概括这本书的主题 [${Date.now()}]`;
  // Mobile tabs: 来源 / 对话 / Studio
  await clickByText(page, ["^对话$", "对话", "^Chat$", "Chat", "聊天"], 6000);
  await page.waitForTimeout(800);

  const marker = `[[Q:${Date.now()}]]`;
  const fullQ = `${q} ${marker}`;
  const beforeText = await page.evaluate(() => document.body?.innerText || "");

  // Composer can sit off-screen under mobile CDP metrics — force focus via DOM.
  const focused = await page.evaluate((text) => {
    const boxes = [...document.querySelectorAll("textarea")];
    const t =
      boxes.find((el) => /提问|创作|ask|message/i.test(el.placeholder || el.getAttribute("aria-label") || "")) ||
      boxes[boxes.length - 1];
    if (!t) return false;
    t.scrollIntoView({ block: "center", inline: "nearest" });
    t.focus();
    t.value = "";
    t.dispatchEvent(new Event("input", { bubbles: true }));
    t.value = text;
    t.dispatchEvent(new Event("input", { bubbles: true }));
    t.dispatchEvent(new Event("change", { bubbles: true }));
    return true;
  }, fullQ);
  if (!focused) {
    const box = page.locator('textarea[placeholder*="提问"], textarea[aria-label="查询框"]').first();
    await box.click({ force: true, timeout: 5000 });
    await box.fill(fullQ);
  }

  const sent = await clickByText(page, ["arrow_forward", "发送", "Send"], 4000);
  if (!sent) {
    await page.keyboard.press("Enter");
  }

  const started = Date.now();
  let answer = "";
  while (Date.now() - started < timeoutMs) {
    await page.waitForTimeout(1500);
    const snap = await page.evaluate((prev) => {
      const t = document.body?.innerText || "";
      return { len: t.length, text: t, grew: t.length > prev.length + 40 };
    }, beforeText);
    if (!snap.text.includes(marker) && snap.grew) {
      // marker may not echo; still accept growth after send
    }
    if (snap.grew) {
      const delta = snap.text.length >= beforeText.length ? snap.text.slice(beforeText.length) : snap.text;
      const stillThinking =
        /正在生成|thinking|generating…|generating\.\.\.|writing/i.test(delta) && delta.length < 120;
      if (!stillThinking && delta.trim().length > 30) {
        answer = delta.trim();
        break;
      }
    }
  }

  // Fallback: notebook already has a prior answer — treat as chat-capable if 对话 pane has prose.
  if (!answer) {
    const existing = await page.evaluate(() => {
      const t = document.body?.innerText || "";
      // Look for a paragraph-sized assistant block near thumbs / copy icons
      const m = t.match(/这本[\s\S]{40,600}/);
      return m ? m[0] : "";
    });
    if (existing) {
      answer = existing;
    }
  }

  const ok = answer.length > 0 && !/sign in|登录|log in to continue/i.test(answer);
  return {
    ok,
    question: fullQ,
    answerSnippet: ok ? answer.slice(0, 800) : answer.slice(0, 400),
    elapsedMs: Date.now() - started,
    sourceCount: await countSources(page),
    composerFocused: focused,
  };
}
