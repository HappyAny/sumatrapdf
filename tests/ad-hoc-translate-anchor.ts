// SPDX-License-Identifier: GPL-3.0-only
// Run: bun tests/ad-hoc-translate-anchor.ts --no-build
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand } from "./control.ts";
import { assemblePdf, cmdId, loadPng, makeTextPdf, pollUntil, runStandalone, tmpPath, writeAppdata } from "./util.ts";
import { findCanvas, killAndWait, launchControlled, pressEscape, sendCommandSync } from "./win-automation.ts";
import {
  captureWindowToPng,
  clientToScreen,
  enumChildWindows,
  enumWindows,
  findTopWindow,
  getClassName,
  getClientRect,
  getControlText,
  getExtendedFrameBounds,
  getScrollInfo,
  getWindowLong,
  getWindowOwner,
  getWindowPid,
  getWindowRect,
  getWorkArea,
  isWindowEnabled,
  isWindowVisible,
  packCoords,
  repaintWindow,
  sendMessage,
  setProcessDpiAware,
  setWindowPos,
} from "./winapi.ts";
import { comboItems, controlById, selectCombo, TranslateControl, translationBody } from "./translate-test-controls.ts";

const TIMEOUT = 8000;
const CALLOUT_GAP_DIP = 12;
const COPY_BUTTON = 3001;
const BM_CLICK = 0x00f5;
const CB_GETCURSEL = 0x0147;
const WM_NCHITTEST = 0x0084;
const HTTRANSPARENT = -1n;
const WM_MOUSEACTIVATE = 0x0021;
const MA_NOACTIVATE = 3n;
const WM_VSCROLL = 0x0115;
const WM_HSCROLL = 0x0114;
const SB_LINERIGHT = 1;
const SB_PAGEDOWN = 3;
const SB_TOP = 6;
const SB_BOTTOM = 7;
const WM_LBUTTONDOWN = 0x0201;
const WM_LBUTTONUP = 0x0202;
const MK_LBUTTON = 0x0001;
const SWP_NOSIZE = 0x0001;
const SWP_NOZORDER = 0x0004;
const SWP_NOREDRAW = 0x0008;
const SWP_NOACTIVATE = 0x0010;
const GWL_EXSTYLE = -20;
const WS_EX_TOPMOST = 0x00000008;
const WS_EX_TRANSPARENT = 0x00000020;
const WS_EX_TOOLWINDOW = 0x00000080;
const WS_EX_LAYERED = 0x00080000;
const WS_EX_NOACTIVATE = 0x08000000;
const ARROW_CLASS = "SumatraTranslateArrow";
const TOOLBAR_CLASS = "SumatraSelectionToolbar";
const TEXT = "选中文本的翻译应保持可见，并随页面移动。";
const LONG_TEXT = (TEXT + "\n").repeat(48);
const SOURCE_LINES = Array.from(
  { length: 12 },
  (_, i) => `Line ${i + 1} of the selected passage for the reading card.`,
);
const SOURCE = SOURCE_LINES.join("\n");
const MODEL = "anchor-test-model";
const KEY = "anchor-test-not-a-secret";
const POSITIONS = { auto: "自动", top: "上方", bottom: "下方", left: "左侧", right: "右侧" } as const;
type Position = keyof typeof POSITIONS;
type Side = Exclude<Position, "auto">;
type Session = Awaited<ReturnType<typeof launchControlled>>;
type Rect = { left: number; top: number; right: number; bottom: number };
type Point = { x: number; y: number };
type State = {
  raw: string;
  visible: boolean;
  busy: boolean;
  error: boolean;
  pinned: boolean;
  request: number;
  text: string;
  dpi: number;
  anchorVisible: boolean;
  anchor: Rect;
  position: Position;
  side: Side;
  arrowVisible: boolean;
  arrowRect: Rect;
  arrowTip: Point;
};
type Form = { hwnd: number; field: (name: keyof typeof TranslateControl) => number };
type MockRequest = { model: string; stream: boolean };
type Context = { session: Session; received: MockRequest[]; appdata: string; pdf: string };
type Reply = () => Response;

function assert(value: unknown, message: string): asserts value {
  if (!value) throw new Error(`translate-anchor: ${message}`);
}

function parseRect(value = ""): Rect {
  const [x = 0, y = 0, w = 0, h = 0] = value.split(/[ ,]+/).map(Number);
  return { left: x, top: y, right: x + w, bottom: y + h };
}

async function state(client: ControlClient, action = ""): Promise<State> {
  const reply = await client.request(ControlCommand.TestTranslatePopup, action ? [action] : []);
  assert(reply[0] === 0, `popup diagnostic failed: ${String(reply[1] ?? "")}`);
  const raw = String(reply[1] ?? "");
  const fields = Object.fromEntries(
    raw.split(/\r?\n/).flatMap((line) => {
      const at = line.indexOf("=");
      return at < 0 ? [] : [[line.slice(0, at), line.slice(at + 1)]];
    }),
  );
  const [x = 0, y = 0] = (fields.arrowTip || "").split(/[ ,]+/).map(Number);
  return {
    raw,
    visible: fields.visible === "1",
    busy: fields.busy === "1",
    error: fields.error === "1",
    pinned: fields.pinned === "1",
    request: Number(fields.request || 0),
    text: JSON.parse(fields.text || '""'),
    dpi: Number(fields.dpi || 96),
    anchorVisible: fields.anchorVisible === "1",
    anchor: parseRect(fields.anchor),
    position: fields.position as Position,
    side: fields.side as Side,
    arrowVisible: fields.arrowVisible === "1",
    arrowRect: parseRect(fields.arrowRect),
    arrowTip: { x, y },
  };
}

async function waitState(session: Session, predicate: (value: State) => boolean, error: string): Promise<State> {
  return pollUntil(() => state(session.client), predicate, {
    timeoutMs: TIMEOUT,
    intervalMs: 20,
    error: (last) => `${error}\n${last?.raw ?? "no state"}`,
  });
}

async function expectText(session: Session, text = TEXT): Promise<State> {
  const value = await waitState(
    session,
    (current) => current.visible && !current.busy && current.text === text,
    "translation did not finish with expected text",
  );
  assert(!value.error, "valid translation was marked as failed");
  return value;
}

async function drag(session: Session, text = "quick"): Promise<void> {
  const reply = await session.client.request(ControlCommand.TestRotatedTextMouseDrag, [text]);
  const expected = text.replaceAll("\n", " ");
  assert(
    String(reply[1] ?? "").includes(`selected=${expected}\n`),
    `native selection failed: ${String(reply[1] ?? "")}`,
  );
}

function popup(session: Session): number {
  const hwnd = findTopWindow(session.proc.pid!, "SumatraTranslatePopup");
  assert(hwnd && isWindowVisible(hwnd), "native translation popup is missing");
  return hwnd;
}

function arrowWindows(session: Session): number[] {
  const found: number[] = [];
  enumWindows((hwnd) => {
    if (getWindowPid(hwnd) === session.proc.pid && getClassName(hwnd) === ARROW_CLASS) found.push(hwnd);
    return true;
  });
  return found;
}

function noArrow(session: Session): void {
  assert(
    arrowWindows(session).every((hwnd) => !isWindowVisible(hwnd)),
    "a stale native arrow remains visible",
  );
}

function sameRect(a: Rect, b: Rect): boolean {
  return a.left === b.left && a.top === b.top && a.right === b.right && a.bottom === b.bottom;
}

function inside(a: Rect, b: Rect, tolerance = 0): boolean {
  return (
    a.left >= b.left - tolerance &&
    a.top >= b.top - tolerance &&
    a.right <= b.right + tolerance &&
    a.bottom <= b.bottom + tolerance
  );
}

function overlaps(a: Rect, b: Rect): boolean {
  return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

function canvasRect(session: Session): Rect {
  const canvas = findCanvas(session.frame);
  assert(canvas && canvas !== session.frame, "native document canvas is missing");
  const client = getClientRect(canvas);
  const origin = clientToScreen(canvas, client.left, client.top);
  return {
    left: origin.x,
    top: origin.y,
    right: origin.x + client.right - client.left,
    bottom: origin.y + client.bottom - client.top,
  };
}

function visibleToolbar(session: Session): number {
  const hwnd = findTopWindow(session.proc.pid!, TOOLBAR_CLASS);
  return hwnd && isWindowVisible(hwnd) ? hwnd : 0;
}

function capture(hwnd: number, path: string): void {
  repaintWindow(hwnd);
  enumChildWindows(hwnd, (child) => {
    repaintWindow(child);
    return true;
  });
  assert(captureWindowToPng(hwnd, path), `native PrintWindow failed: ${path}`);
}

function checkTriangle(hwnd: number, side: Side, tag: string): void {
  const path = tmpPath(`translate-anchor-${tag}-arrow.png`);
  capture(hwnd, path);
  const image = loadPng(path);
  const horizontal = side === "left" || side === "right";
  const length = horizontal ? image.w : image.h;
  const cross = horizontal ? image.h : image.w;
  const countAt = (fraction: number): number => {
    const tipAtStart = side === "right" || side === "bottom";
    const axis = Math.min(length - 1, Math.max(0, Math.round((tipAtStart ? fraction : 1 - fraction) * (length - 1))));
    let count = 0;
    for (let i = 0; i < cross; i++) {
      const at = (horizontal ? i * image.w + axis : axis * image.w + i) * image.nComp;
      const rgb = [image.data[at]!, image.data[at + 1]!, image.data[at + 2]!];
      if (Math.min(...rgb) >= 150 && Math.max(...rgb) - Math.min(...rgb) <= 35) count++;
    }
    return count;
  };
  const nearTip = countAt(0.2);
  const nearBase = countAt(0.8);
  assert(
    nearTip > 0 && nearBase > nearTip + Math.max(2, Math.floor(cross / 8)),
    `native arrow pixels do not form a triangle pointing ${side}: tip=${nearTip}, base=${nearBase}, ${image.w}x${image.h}`,
  );
  writeFileSync(
    tmpPath(`translate-anchor-${tag}-pixels.json`),
    JSON.stringify({ side, width: image.w, height: image.h, nearTip, nearBase }, null, 2),
  );
}

function checkCallout(session: Session, current: State, tag?: string): void {
  assert(current.anchorVisible && current.arrowVisible, `visible selection lacks an arrow:\n${current.raw}`);
  assert(
    current.position in POSITIONS && ["top", "bottom", "left", "right"].includes(current.side),
    `invalid placement diagnostics:\n${current.raw}`,
  );
  assert(
    current.anchor.right > current.anchor.left &&
      current.anchor.bottom > current.anchor.top &&
      inside(current.anchor, canvasRect(session)),
    "anchor was not clipped to the real canvas viewport",
  );
  const bubble = popup(session);
  const arrows = arrowWindows(session).filter(isWindowVisible);
  assert(arrows.length === 1, `expected one native arrow HWND, got ${arrows.length}`);
  const arrow = arrows[0]!;
  assert(getWindowOwner(arrow) === bubble, "arrow is not owned by the translation popup");
  const style = getWindowLong(arrow, GWL_EXSTYLE);
  const required = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
  assert(
    (style & required) === required && !(style & WS_EX_TOPMOST),
    "arrow window styles can activate or cover other apps",
  );
  assert(
    sendMessage(arrow, WM_NCHITTEST, 0, packCoords(current.arrowTip.x, current.arrowTip.y)) === HTTRANSPARENT,
    "arrow is not click-through",
  );
  assert(sendMessage(arrow, WM_MOUSEACTIVATE, 0, 0) === MA_NOACTIVATE, "arrow can activate the reader");
  const actualArrow = getWindowRect(arrow);
  assert(sameRect(actualArrow, current.arrowRect), "arrow diagnostic rectangle differs from its HWND");
  assert(
    inside(getWindowRect(bubble), canvasRect(session)) &&
      inside(actualArrow, canvasRect(session)) &&
      inside(getWindowRect(bubble), getWorkArea()) &&
      inside(actualArrow, getWorkArea()),
    "callout escaped the visible document area",
  );
  const visible = getExtendedFrameBounds(bubble);
  assert(visible.right > visible.left && visible.bottom > visible.top, "DWM did not return the visible popup border");
  const { anchor: a, arrowTip: tip, side } = current;
  const tolerance = Math.max(5, Math.round(current.dpi / 32));
  if (side === "right") {
    assert(
      visible.left >= a.right &&
        Math.abs(tip.x - a.right) <= 2 &&
        tip.y >= a.top &&
        tip.y <= a.bottom &&
        Math.abs(actualArrow.right - visible.left) <= tolerance,
      "right arrow misses selection or visible popup edge",
    );
  } else if (side === "left") {
    assert(
      visible.right <= a.left &&
        Math.abs(tip.x - a.left) <= 2 &&
        tip.y >= a.top &&
        tip.y <= a.bottom &&
        Math.abs(actualArrow.left - visible.right) <= tolerance,
      "left arrow misses selection or visible popup edge",
    );
  } else if (side === "bottom") {
    assert(
      visible.top >= a.bottom &&
        Math.abs(tip.y - a.bottom) <= 2 &&
        tip.x >= a.left &&
        tip.x <= a.right &&
        Math.abs(actualArrow.bottom - visible.top) <= tolerance,
      "bottom arrow misses selection or visible popup edge",
    );
  } else {
    assert(
      visible.bottom <= a.top &&
        Math.abs(tip.y - a.top) <= 2 &&
        tip.x >= a.left &&
        tip.x <= a.right &&
        Math.abs(actualArrow.top - visible.bottom) <= tolerance,
      "top arrow misses selection or visible popup edge",
    );
  }
  assert(
    inside({ left: tip.x, top: tip.y, right: tip.x + 1, bottom: tip.y + 1 }, actualArrow),
    "native arrow rectangle does not include its tip",
  );
  if (tag) {
    checkTriangle(arrow, side, tag);
    capture(bubble, tmpPath(`translate-anchor-${tag}-popup.png`));
    writeFileSync(tmpPath(`translate-anchor-${tag}-state.txt`), current.raw);
  }
}

function assertSideRoom(session: Session, current: State, side: Side): void {
  const canvas = canvasRect(session);
  const card = getWindowRect(popup(session));
  const gap = Math.max(1, Math.round((CALLOUT_GAP_DIP * current.dpi) / 96));
  const { anchor } = current;
  const room = {
    left: anchor.left - canvas.left,
    right: canvas.right - anchor.right,
    top: anchor.top - canvas.top,
    bottom: canvas.bottom - anchor.bottom,
  };
  const size = side === "left" || side === "right" ? card.right - card.left : card.bottom - card.top;
  assert(room[side] >= size + gap, `fixture has ${room[side]} px on ${side}, needs ${size + gap} px`);
}

async function openConfig(session: Session): Promise<Form> {
  await state(session.client, "settings");
  const hwnd = await pollUntil(
    () => findTopWindow(session.proc.pid!, "SumatraTranslateConfig"),
    (value) => !!value && isWindowVisible(value),
    { timeoutMs: TIMEOUT, error: "settings did not open" },
  );
  return { hwnd, field: (name) => controlById(hwnd, TranslateControl[name]) };
}

async function closeConfig(form: Form, save: boolean): Promise<void> {
  sendMessage(form.field(save ? "save" : "cancel"), BM_CLICK, 0, 0);
  await pollUntil(
    () => isWindowVisible(form.hwnd),
    (value) => !value,
    { timeoutMs: TIMEOUT, error: "settings did not close" },
  );
}

async function setPosition(session: Session, position: Position): Promise<State> {
  const form = await openConfig(session);
  selectCombo(form.field("position"), new RegExp(`^${POSITIONS[position]}$`));
  await closeConfig(form, true);
  return waitState(session, (value) => value.position === position, "display placement did not apply");
}

async function pin(session: Session): Promise<void> {
  if (!(await state(session.client)).pinned) await state(session.client, "pin");
  assert((await state(session.client)).pinned, "pin action did not enable fixed state");
}

function unchanged(before: State, after: State, received: MockRequest[], count: number): void {
  assert(
    after.visible &&
      after.text === before.text &&
      after.request === before.request &&
      after.busy === before.busy &&
      after.pinned === before.pinned &&
      received.length === count,
    `placement or viewport change altered translation state:\n${before.raw}\n${after.raw}`,
  );
}

async function scrollState(session: Session): Promise<{ page: number; y: number; raw: string }> {
  const reply = await session.client.request(ControlCommand.TestScrollState);
  const raw = String(reply[1] ?? "");
  const match = /^OK page=(\d+) x=\S+ y=(\S+)/.exec(raw);
  assert(reply[0] === 0 && match, `native scroll state is invalid: ${raw}`);
  return { page: Number(match[1]), y: Number(match[2]), raw };
}

async function scrollAway(session: Session): Promise<State> {
  const canvas = findCanvas(session.frame);
  const before = await scrollState(session);
  const initial = getScrollInfo(canvas);
  assert(initial.max - initial.min + 1 > initial.page, "fixture does not have a scrollable canvas");
  sendMessage(canvas, WM_VSCROLL, SB_PAGEDOWN, 0);
  let moved = await scrollState(session);
  assert(
    moved.page === 1 && moved.y > before.y && getScrollInfo(canvas).pos > initial.pos,
    `WM_VSCROLL did not move within the same page: ${before.raw} -> ${moved.raw}`,
  );
  let current = await state(session.client);
  if (current.anchorVisible) {
    sendMessage(canvas, WM_VSCROLL, SB_BOTTOM, 0);
    moved = await scrollState(session);
  }
  assert(moved.page === 1 && moved.y > before.y, "offscreen fixture navigated away from its only page");
  current = await waitState(
    session,
    (value) => !value.anchorVisible,
    "selected word outside viewport is still anchored",
  );
  assert(current.visible && !current.arrowVisible, "scrolling selection offscreen hid the popup or retained its arrow");
  noArrow(session);
  await session.client.waitForRenderIdle();
  return current;
}

async function scrollBack(session: Session): Promise<State> {
  sendMessage(findCanvas(session.frame), WM_VSCROLL, SB_TOP, 0);
  const current = await waitState(
    session,
    (value) => value.anchorVisible && value.arrowVisible,
    "returning selected text did not restore its arrow",
  );
  assert((await scrollState(session)).page === 1, "returning selection changed page");
  return current;
}

function clearSelection(session: Session): void {
  const canvas = findCanvas(session.frame);
  const rect = getClientRect(canvas);
  const point = packCoords(Math.round((rect.left + rect.right) / 2), Math.round((rect.top + rect.bottom) / 2));
  sendMessage(canvas, WM_LBUTTONDOWN, MK_LBUTTON, point);
  sendMessage(canvas, WM_LBUTTONUP, 0, point);
}

async function quit(session: Session): Promise<void> {
  if (session.proc.exitCode !== null) {
    session.client.close();
    return;
  }
  try {
    await session.client.quit();
    const code = await Promise.race([
      session.proc.exited,
      Bun.sleep(TIMEOUT).then(() => {
        throw new Error("reader exit timed out");
      }),
    ]);
    assert(code === 0, "reader exit failed");
  } catch (error) {
    session.client.close();
    await killAndWait(session.proc);
    throw error;
  }
}

function delta(text: string, finish: string | null = null): string {
  return `data: ${JSON.stringify({ choices: [{ delta: { content: text }, finish_reason: finish }] })}\r\n\r\n`;
}

function gate(): { promise: Promise<void>; release: () => void } {
  let release!: () => void;
  const promise = new Promise<void>((resolve) => {
    release = resolve;
  });
  return { promise, release };
}

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const pdf = tmpPath("translate-anchor.pdf");
  const roomyPdf = tmpPath("translate-anchor-roomy.pdf");
  const secondPdf = tmpPath("translate-anchor-second.pdf");
  const sourcePdf = tmpPath("translate-anchor-long-source.pdf");
  writeFileSync(pdf, makeTextPdf("The quick brown fox reads a book."));
  writeFileSync(roomyPdf, makeTextPdf("A short lead. quick brown fox reads a book."));
  writeFileSync(secondPdf, makeTextPdf("The other document has no selection."));
  const content = `BT /F1 11 Tf 24 TL 72 613 Td ${SOURCE_LINES.map((line) => `(${line}) Tj`).join(" T* ")} ET`;
  writeFileSync(
    sourcePdf,
    assemblePdf([
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      `<< /Length ${content.length} >>\nstream\n${content}\nendstream`,
    ]),
  );
  const routes = new Map<string, { received: MockRequest[]; reply: Reply }>();
  const results: { name: string; status: string; elapsedMs: number; reason?: string }[] = [];
  let ordinal = 0;
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    async fetch(request) {
      const route = routes.get(new URL(request.url).pathname);
      if (!route) return new Response("unknown anchor route", { status: 404 });
      const body = await request.json();
      route.received.push({ model: body.model, stream: body.stream });
      return route.reply();
    },
  });

  async function run(
    name: string,
    check: (context: Context) => Promise<void>,
    options: {
      reply?: Reply;
      saveSettings?: boolean;
      twoTabs?: boolean;
      roomy?: boolean;
      fixture?: { pdf: string; zoom: string; selection: string };
      settings?: Record<string, string>;
    } = {},
  ): Promise<void> {
    if (process.env.SUMATRA_ANCHOR_CASE && process.env.SUMATRA_ANCHOR_CASE !== name) return;
    const started = Date.now();
    const path = `/${++ordinal}-${name}`;
    const received: MockRequest[] = [];
    routes.set(path, {
      received,
      reply:
        options.reply ??
        (() =>
          Response.json({
            choices: [{ message: { content: TEXT }, finish_reason: "stop" }],
          })),
    });
    const appdata = writeAppdata(
      `translate-anchor-${ordinal}`,
      [
        "RestoreSession = false",
        "CheckForUpdates = false",
        "ShowToc = false",
        "UseTabs = true",
        "NoHomeTab = true",
        "DefaultDisplayMode = continuous",
        "DefaultZoom = 200%",
        "TranslatePopupEnabled = true",
        `TranslatePopupUrl = ${server.url.origin}${path}`,
        `TranslatePopupModel = ${MODEL}`,
        `TranslatePopupApiKey = ${KEY}`,
        ...Object.entries({
          SelectionToolbar: "false",
          TranslatePopupAutoSize: "false",
          TranslatePopupWidth: "330",
          TranslatePopupHeight: "180",
          ...options.settings,
        }).map(([key, value]) => `${key} = ${value}`),
      ].join("\n"),
    );
    const work = getWorkArea();
    const margin = Math.round((work.right - work.left) * 0.08);
    const roomySize = `${work.right - work.left - 2 * margin}x${Math.floor((work.bottom - work.top) * 0.84)}`;
    const position = options.roomy ? ["-window-pos", `${roomySize}@${work.left + margin}x${work.top}`] : [];
    const document = options.fixture?.pdf ?? (options.roomy ? roomyPdf : pdf);
    const session = await launchControlled(
      [
        "-appdata",
        appdata,
        "-zoom",
        options.fixture?.zoom ?? "200",
        ...position,
        ...(options.twoTabs ? [secondPdf] : []),
        document,
      ],
      { saveSettings: options.saveSettings },
    );
    try {
      await session.client.waitForRenderIdle();
      await session.client.setNotificationsEnabled(false);
      sendMessage(findCanvas(session.frame), WM_HSCROLL, SB_TOP, 0);
      sendMessage(findCanvas(session.frame), WM_VSCROLL, SB_TOP, 0);
      await session.client.waitForRenderIdle();
      await drag(session, options.fixture?.selection);
      await check({ session, received, appdata, pdf });
      await quit(session);
      assert(arrowWindows(session).length === 0, "reader quit left an orphan arrow HWND");
      results.push({ name, status: "PASS", elapsedMs: Date.now() - started });
      console.log(`PASS: translate-anchor ${name}`);
    } catch (error) {
      results.push({ name, status: "FAIL", elapsedMs: Date.now() - started, reason: String(error) });
      console.error(`FAIL: translate-anchor ${name}: ${String(error)}`);
      if (session.proc.exitCode === null) {
        try {
          writeFileSync(tmpPath(`translate-anchor-${name}-failure-state.txt`), (await state(session.client)).raw);
        } catch {
          /* The original assertion remains the failure. */
        }
        for (const cls of ["SumatraTranslatePopup", "SumatraTranslateConfig", ARROW_CLASS]) {
          const hwnd = findTopWindow(session.proc.pid!, cls);
          if (hwnd) {
            try {
              capture(hwnd, tmpPath(`translate-anchor-${name}-${cls}.png`));
            } catch {
              /* Keep other evidence. */
            }
          }
        }
      }
      session.client.close();
      await killAndWait(session.proc);
    } finally {
      routes.delete(path);
      writeFileSync(tmpPath("translate-anchor-results.json"), JSON.stringify(results, null, 2));
    }
  }

  try {
    await run(
      "toolbar-and-reader-bounds",
      async ({ session, received }) => {
        const before = await expectText(session, LONG_TEXT);
        const canvas = canvasRect(session);
        const frame = getWindowRect(session.frame);
        assert(canvas.left - frame.left > 80, "fixture did not open a docked sidebar");
        assert(canvas.top > frame.top, "fixture did not display reader chrome above the canvas");
        const toolbar = visibleToolbar(session);
        assert(toolbar, "selection shortcuts were not available with room to show both");
        const bar = getWindowRect(toolbar);
        const card = getWindowRect(popup(session));
        const containment = inside(card, canvas);
        const collision = overlaps(card, bar);
        writeFileSync(
          tmpPath("translate-anchor-toolbar-layout.json"),
          JSON.stringify({ canvas, card, bar, containment, collision }, null, 2),
        );
        assert(
          containment && !collision,
          `card escaped reader or covered shortcuts: ${JSON.stringify({ canvas, card, bar })}`,
        );
        if (before.arrowVisible) {
          assert(
            inside(before.arrowRect, canvas) && !overlaps(before.arrowRect, bar),
            "arrow crossed reader or toolbar",
          );
        }
        const copied = await session.client.request(ControlCommand.TestSelectionToolbar, ["CmdCopySelection"]);
        assert(copied[0] === 0, `original Copy action failed: ${copied[1]}`);
        unchanged(before, await state(session.client), received, 1);

        sendCommandSync(session.frame, cmdId("CmdToggleSidebarAutoHide"));
        const layout = await pollUntil(
          () => session.client.layout(),
          (value) => !value.items.sidebarBottom?.visible,
          { timeoutMs: TIMEOUT, error: "favorites did not become an auto-hide sidebar" },
        );
        const view = layout.items.canvas!.rect;
        const reveal = await session.client.request(ControlCommand.TestSidebarCursor, [
          view.x + 2,
          (view.y + view.dy / 2) | 0,
        ]);
        assert(reveal[0] === 0, "could not reveal sidebar overlay");
        const shown = await pollUntil(
          () => session.client.layout(),
          (value) => !!value.items.sidebarBottom?.visible,
          { timeoutMs: TIMEOUT, error: "sidebar overlay did not reveal" },
        );
        const panel = shown.items.sidebarBottom!.rect;
        const origin = clientToScreen(session.frame, panel.x, panel.y);
        const overlay = { left: origin.x, top: origin.y, right: origin.x + panel.dx, bottom: origin.y + panel.dy };
        const overlayCard = getWindowRect(popup(session));
        assert(
          inside(overlayCard, canvasRect(session)) && !overlaps(overlayCard, overlay),
          "popup covered the floating sidebar",
        );
        unchanged(before, await state(session.client), received, 1);
        await session.client.request(ControlCommand.TestSidebarCursor, [view.x + view.dx - 10, view.y + 12]);
        await pollUntil(
          () => session.client.layout(),
          (value) => !value.items.sidebarBottom?.visible,
          { timeoutMs: TIMEOUT, error: "sidebar overlay did not hide" },
        );
        writeFileSync(
          tmpPath("translate-anchor-sidebar-overlay.json"),
          JSON.stringify({ overlay, overlayCard }, null, 2),
        );

        const away = await scrollAway(session);
        unchanged(before, away, received, 1);
        assert(inside(getWindowRect(popup(session)), canvasRect(session)), "unanchored popup escaped reader");
        sendMessage(findCanvas(session.frame), WM_VSCROLL, SB_TOP, 0);
        const back = await waitState(
          session,
          (value) => value.anchorVisible,
          "restored selection did not locate again",
        );
        unchanged(before, back, received, 1);
        assert(inside(getWindowRect(popup(session)), canvasRect(session)), "returning selection escaped reader");
      },
      {
        settings: {
          SelectionToolbar: "true",
          ShowFavorites: "true",
          SidebarDx: "280",
          TranslatePopupWidth: "1600",
          TranslatePopupHeight: "1200",
        },
        reply: () => Response.json({ choices: [{ message: { content: LONG_TEXT }, finish_reason: "stop" }] }),
      },
    );

    await run(
      "toolbar-restores-after-close",
      async ({ session, received }) => {
        const before = await expectText(session);
        const toolbar = visibleToolbar(session);
        assert(toolbar, "fixture needs shortcuts to be visible before resizing");
        const frame = getWindowRect(session.frame);
        const canvas = canvasRect(session);
        const bar = getWindowRect(toolbar);
        const minimum = Math.round((180 * before.dpi) / 96);
        assert(bar.top - canvas.top < minimum, "fixture has room for the card above the toolbar");
        const availableBelow = Math.round(minimum * 0.78);
        assert(bar.bottom + availableBelow >= before.anchor.bottom, "fixture would scroll selection out of view");
        const newBottom = bar.bottom + availableBelow + frame.bottom - canvas.bottom;
        assert(newBottom < frame.bottom, "fixture needs to shrink the reader");
        assert(
          setWindowPos(
            session.frame,
            frame.left,
            frame.top,
            frame.right - frame.left,
            newBottom - frame.top,
            SWP_NOZORDER | SWP_NOACTIVATE,
          ),
          "reader resize failed",
        );
        await session.client.waitForRenderIdle();
        const current = await state(session.client);
        const constrained = canvasRect(session);
        const stillPlaced = getWindowRect(toolbar);
        assert(
          current.anchorVisible && inside(getWindowRect(popup(session)), constrained),
          "short reader lost the visible selection or popup",
        );
        assert(
          stillPlaced.top - constrained.top < minimum && constrained.bottom - stillPlaced.bottom < minimum,
          "short reader still has vertical room to display both windows",
        );
        assert(!visibleToolbar(session), "toolbar stayed visible underneath the occupying translation");
        unchanged(before, current, received, 1);
        await state(session.client, "close");
        assert(
          visibleToolbar(session) === toolbar,
          "closing the occupying popup did not restore the original shortcuts",
        );
        assert(received.length === 1, "restoring original shortcuts repeated translation");
      },
      { settings: { SelectionToolbar: "true", TranslatePopupWidth: "1600", TranslatePopupHeight: "1200" } },
    );

    await run(
      "default-arrow",
      async ({ session, received }) => {
        const current = await expectText(session);
        assert(current.position === "auto", `default placement is not auto:\n${current.raw}`);
        assertSideRoom(session, current, "right");
        assert(current.side === "right", "auto placement did not prefer the available right side");
        checkCallout(session, current, "default");
        assert(received.length === 1 && received[0]!.model === MODEL, "default arrow changed the translation request");
      },
      { roomy: true },
    );

    const startTranslation = gate();
    const finishTranslation = gate();
    const firstChunk = "短译文。";
    const translated = "\n" + firstChunk;
    try {
      await run(
        "source-preview-placement",
        async ({ session, received }) => {
          const before = await waitState(session, (value) => value.visible && value.busy, "translation did not start");
          const hwnd = popup(session);
          const edit = translationBody(hwnd);
          const copy = controlById(hwnd, COPY_BUTTON);
          const initialRect = getWindowRect(hwnd);
          assert(
            getControlText(edit).replaceAll("\r\n", "\n") === SOURCE,
            `waiting card did not show the selected text:\n${before.raw}`,
          );
          assert(!isWindowEnabled(copy), "waiting card enabled the copy-translation button for the source text");
          assert(before.side !== "top", `first placement used the top slot despite the tall source:\n${before.raw}`);
          checkCallout(session, before, "source-preview");

          startTranslation.release();
          const partial = await waitState(
            session,
            (value) => value.busy && value.text === translated,
            "first translation chunk did not replace the source preview",
          );
          assert(
            getControlText(edit).replaceAll("\r\n", "\n") === translated,
            "translated text did not replace the native body",
          );
          assert(isWindowEnabled(copy), "copy stayed disabled after translation arrived");
          // Wait through a streaming resize timer before releasing the terminal event.
          await Bun.sleep(250);
          assert(
            partial.request === before.request && sameRect(initialRect, getWindowRect(hwnd)),
            "short first chunk moved the waiting card",
          );
          finishTranslation.release();
          const done = await expectText(session, translated);
          assert(done.request === before.request && received.length === 1, "preview started an extra request");
          assertSideRoom(session, done, "top");
          assert(
            done.side === "top" &&
              getWindowRect(hwnd).bottom - getWindowRect(hwnd).top < initialRect.bottom - initialRect.top,
            "completed short translation did not fit the preferred top slot",
          );
          checkCallout(session, done, "source-preview-translated");
        },
        {
          roomy: true,
          fixture: { pdf: sourcePdf, zoom: "100", selection: SOURCE },
          settings: { TranslatePopupAutoSize: "true", TranslatePopupFontSize: "16", TranslatePopupPosition: "top" },
          reply: () =>
            new Response(
              new ReadableStream({
                async start(controller) {
                  controller.enqueue(Buffer.from(delta("\n")));
                  await startTranslation.promise;
                  controller.enqueue(Buffer.from(delta(firstChunk)));
                  await finishTranslation.promise;
                  controller.enqueue(Buffer.from(delta("", "stop")));
                  controller.close();
                },
              }),
              { headers: { "Content-Type": "text/event-stream" } },
            ),
        },
      );
    } finally {
      startTranslation.release();
      finishTranslation.release();
    }

    await run(
      "four-preferred-sides",
      async ({ session, received }) => {
        const before = await expectText(session);
        const form = await openConfig(session);
        assert(
          JSON.stringify(comboItems(form.field("position"))) === JSON.stringify(Object.values(POSITIONS)),
          "position choices differ from 自动/上方/下方/左侧/右侧",
        );
        await closeConfig(form, false);
        for (const side of ["top", "bottom", "left", "right"] as const) {
          assertSideRoom(session, before, side);
          const current = await setPosition(session, side);
          assert(current.side === side, `explicit ${side} fell back despite a centered anchor`);
          unchanged(before, current, received, 1);
          checkCallout(session, current, side);
        }
      },
      { roomy: true },
    );

    await run("offscreen-finished", async ({ session, received }) => {
      const before = await expectText(session);
      checkCallout(session, before);
      const last = getWindowRect(popup(session));
      const away = await scrollAway(session);
      unchanged(before, away, received, 1);
      assert(sameRect(last, getWindowRect(popup(session))), "offscreen selection moved the last popup position");
      const back = await scrollBack(session);
      unchanged(before, back, received, 1);
      checkCallout(session, back, "returned");
      writeFileSync(tmpPath("translate-anchor-scroll.json"), JSON.stringify({ before, away, back }, null, 2));
    });

    const more = gate();
    const finish = gate();
    let canceled = false;
    try {
      await run(
        "offscreen-stream",
        async ({ session, received }) => {
          const before = await waitState(
            session,
            (value) => value.visible && value.busy && value.text === "第一段。",
            "first stream token was not visible while request stayed open",
          );
          checkCallout(session, before);
          const last = getWindowRect(popup(session));
          const away = await scrollAway(session);
          unchanged(before, away, received, 1);
          more.release();
          const updated = await waitState(
            session,
            (value) => value.busy && value.text === "第一段。第二段。",
            "in-flight translation stopped updating offscreen",
          );
          assert(
            updated.visible &&
              !updated.anchorVisible &&
              !updated.arrowVisible &&
              !canceled &&
              updated.request === before.request &&
              received.length === 1 &&
              sameRect(last, getWindowRect(popup(session))),
            "offscreen streaming canceled, retried, hid, or moved the popup",
          );
          noArrow(session);
          const back = await scrollBack(session);
          checkCallout(session, back, "stream-returned");
          assert(
            back.busy && back.text === updated.text && back.request === before.request && received.length === 1,
            "returning anchor restarted the in-flight request",
          );
          finish.release();
          const done = await expectText(session, "第一段。第二段。完成。");
          assert(done.request === before.request && received.length === 1 && !canceled, "stream did not finish once");
        },
        {
          reply: () =>
            new Response(
              new ReadableStream<Uint8Array>({
                async start(controller) {
                  try {
                    controller.enqueue(Buffer.from(delta("第一段。")));
                    await more.promise;
                    if (canceled) return;
                    controller.enqueue(Buffer.from(delta("第二段。")));
                    await finish.promise;
                    if (canceled) return;
                    controller.enqueue(Buffer.from(delta("完成。", "stop")));
                    controller.close();
                  } catch (error) {
                    if (!canceled) controller.error(error);
                  }
                },
                cancel() {
                  canceled = true;
                },
              }),
              { headers: { "Content-Type": "text/event-stream" } },
            ),
        },
      );
    } finally {
      more.release();
      finish.release();
    }

    await run(
      "edge-fallback-and-frame-move",
      async ({ session, received }) => {
        const before = await expectText(session);
        const preferred = await setPosition(session, "left");
        assertSideRoom(session, preferred, "left");
        assert(preferred.side === "left", "left preference did not apply");
        const frame = getWindowRect(session.frame);
        const work = getWorkArea();
        const dx = work.left - frame.left;
        assert(dx !== 0, "fixture could not move the reader to the left");
        assert(
          setWindowPos(
            session.frame,
            work.left,
            frame.top,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOACTIVATE,
          ),
          "native reader move failed",
        );
        // No render-idle wait: WM_MOVE must update the popup and arrow itself.
        const moved = await state(session.client);
        assert(moved.anchorVisible && moved.position === "left", "frame move lost the selected anchor/preference");
        assert(moved.side === "left", "moving the frame changed the available placement inside it");
        assert(
          moved.anchor.left === preferred.anchor.left + dx && !sameRect(moved.arrowRect, preferred.arrowRect),
          "frame movement did not update the real selection and arrow position",
        );
        unchanged(before, moved, received, 1);
        checkCallout(session, moved);
        const canvas = findCanvas(session.frame);
        let atEdge = moved;
        for (let i = 0; i < 20 && atEdge.side === "left" && atEdge.anchorVisible; i++) {
          sendMessage(canvas, WM_HSCROLL, SB_LINERIGHT, 0);
          await session.client.waitForRenderIdle();
          atEdge = await state(session.client);
        }
        assert(atEdge.anchorVisible && atEdge.side !== "left", "canvas edge did not force a new side");
        unchanged(before, atEdge, received, 1);
        checkCallout(session, atEdge, "fallback");
        sendMessage(canvas, WM_HSCROLL, SB_TOP, 0);
        await session.client.waitForRenderIdle();
        assert(
          setWindowPos(
            session.frame,
            frame.left,
            frame.top,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOACTIVATE,
          ),
          "reader restore failed",
        );
        const restored = await state(session.client);
        assert(
          restored.side === "left" && sameRect(restored.anchor, preferred.anchor),
          "returning room did not restore preferred side",
        );
        unchanged(before, restored, received, 1);
        checkCallout(session, restored);
      },
      { roomy: true },
    );

    await run(
      "lifecycle-no-ghost-arrow",
      async ({ session, received }) => {
        await expectText(session);
        clearSelection(session);
        await waitState(
          session,
          (value) => !value.visible && !value.arrowVisible,
          "unpin clear did not dismiss the callout",
        );
        noArrow(session);
        await drag(session);
        await expectText(session);
        await pin(session);
        clearSelection(session);
        const pinned = await waitState(
          session,
          (value) => value.visible && value.pinned && !value.arrowVisible,
          "pinned clear hid the text or retained a stale arrow",
        );
        assert(!pinned.anchorVisible && pinned.text === TEXT, "pinned clear changed content or kept its old anchor");
        noArrow(session);
        await state(session.client, "close");
        await waitState(session, (value) => !value.visible && !value.arrowVisible, "Close retained the callout");
        noArrow(session);
        await drag(session);
        await expectText(session);
        await pin(session);
        await pressEscape(session.frame);
        await waitState(session, (value) => !value.visible && !value.arrowVisible, "Esc retained a pinned callout");
        noArrow(session);
        await drag(session);
        await expectText(session);
        sendCommandSync(session.frame, cmdId("CmdNextTab"));
        await session.client.waitForRenderIdle();
        await waitState(session, (value) => !value.visible && !value.arrowVisible, "tab switch kept the old arrow");
        noArrow(session);
        sendCommandSync(session.frame, cmdId("CmdNextTab"));
        await session.client.waitForRenderIdle();
        await drag(session);
        await expectText(session);
        sendCommandSync(session.frame, cmdId("CmdCloseCurrentDocument"));
        await session.client.waitForRenderIdle();
        await waitState(session, (value) => !value.visible && !value.arrowVisible, "document close kept its callout");
        noArrow(session);
        assert(received.length >= 1, "lifecycle fixture never translated a selection");
      },
      { twoTabs: true },
    );

    await run(
      "position-cancel-and-persistence",
      async ({ session, received, appdata, pdf }) => {
        const before = await expectText(session);
        let form = await openConfig(session);
        const items = comboItems(form.field("position"));
        assert(
          items[Number(sendMessage(form.field("position"), CB_GETCURSEL, 0, 0))] === POSITIONS.auto,
          "default settings placement is not 自动",
        );
        selectCombo(form.field("position"), /^左侧$/);
        await closeConfig(form, false);
        const canceled = await state(session.client);
        assert(canceled.position === "auto", "Cancel applied the position draft");
        unchanged(before, canceled, received, 1);
        const saved = await setPosition(session, "bottom");
        unchanged(before, saved, received, 1);
        const settingsPath = join(appdata, "SumatraPDF-settings.txt");
        await pollUntil(
          () => (existsSync(settingsPath) ? readFileSync(settingsPath, "utf8") : ""),
          (text) => /TranslatePopupPosition\s*=\s*bottom\b/.test(text),
          { timeoutMs: TIMEOUT, error: "position preference was not persisted" },
        );
        await quit(session);
        const count = received.length;
        const restarted = await launchControlled(["-appdata", appdata, "-zoom", "200", pdf]);
        try {
          await restarted.client.waitForRenderIdle();
          await restarted.client.setNotificationsEnabled(false);
          sendMessage(findCanvas(restarted.frame), WM_HSCROLL, SB_TOP, 0);
          sendMessage(findCanvas(restarted.frame), WM_VSCROLL, SB_TOP, 0);
          await restarted.client.waitForRenderIdle();
          assert(received.length === count, "restart sent a translation without a selection");
          await drag(restarted);
          const reloaded = await expectText(restarted);
          assert(
            reloaded.position === "bottom" && received.length === count + 1,
            "position did not reload with one new selection request",
          );
          checkCallout(restarted, reloaded, "reloaded");
          form = await openConfig(restarted);
          assert(
            comboItems(form.field("position"))[Number(sendMessage(form.field("position"), CB_GETCURSEL, 0, 0))] ===
              POSITIONS.bottom,
            "reloaded position combo differs from actual preference",
          );
          await closeConfig(form, false);
        } finally {
          await quit(restarted);
        }
      },
      { saveSettings: true },
    );
  } finally {
    server.stop(true);
  }
  const failures = results.filter((result) => result.status === "FAIL");
  assert(results.length > 0, "SUMATRA_ANCHOR_CASE did not match a scenario");
  assert(
    failures.length === 0,
    `${failures.length}/${results.length} scenarios failed; see translate-anchor-results.json`,
  );
  console.log(`PASS: translate-anchor ${results.length} scenarios`);
}

if (import.meta.main) await runStandalone(testit);
