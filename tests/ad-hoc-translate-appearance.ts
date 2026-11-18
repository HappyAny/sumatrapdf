// SPDX-License-Identifier: GPL-3.0-only
// Run: bun tests/ad-hoc-translate-appearance.ts --no-build
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand } from "./control.ts";
import { makeTextPdf, pollUntil, runStandalone, tmpPath, writeAppdata } from "./util.ts";
import { findCanvas, killAndWait, launchControlled } from "./win-automation.ts";
import {
  captureWindowToPng,
  clientToScreen,
  enumChildWindows,
  findTopWindow,
  getClassName,
  getClientRect,
  getControlText,
  getFocusedHwnd,
  getWindowRect,
  getWorkArea,
  isWindowVisible,
  postKeyWithModifier,
  repaintWindow,
  sendMessage,
  sendText,
  setProcessDpiAware,
  setWindowPos,
  VK_CONTROL,
} from "./winapi.ts";
import { controlById, ReadingControl, selectCombo, TranslateControl } from "./translate-test-controls.ts";

const TIMEOUT = 8000;
const BM_CLICK = 0x00f5;
const BM_GETCHECK = 0x00f0;
const WM_KEYDOWN = 0x0100;
const VK_TAB = 0x09;
const VK_RETURN = 0x0d;
const VK_ESCAPE = 0x1b;
const WM_ENTERSIZEMOVE = 0x0231;
const WM_EXITSIZEMOVE = 0x0232;
const WM_MOUSEWHEEL = 0x020a;
const EM_GETLINECOUNT = 0x00ba;
const EM_GETFIRSTVISIBLELINE = 0x00ce;
const EM_LINESCROLL = 0x00b6;
const SHORT = "简短译文。";
const LONG = ("这是一段用于验证浮窗换行与高度上限的译文，包含中文、公式 α≤β 和参考文献 [12]。".repeat(7) + "\n").repeat(
  15,
);
const MODEL = "appearance-test-model";
const KEY = "appearance-test-not-a-secret";
type Session = Awaited<ReturnType<typeof launchControlled>>;
type Form = { hwnd: number; field: (name: keyof typeof TranslateControl) => number };
type State = {
  visible: boolean;
  busy: boolean;
  error: boolean;
  text: string;
  status: string;
  request: number;
  fontSize: number;
  fontPixels: number;
  autoSize: boolean;
  preferredWidth: number;
  preferredHeight: number;
  dpi: number;
  resizeCount: number;
};
type Request = { body: any; url: string };
type Context = { session: Session; received: Request[]; endpoint: string; appdata: string };
type Reply = (request: Request, ordinal: number) => Response;

function assert(value: unknown, message: string): asserts value {
  if (!value) throw new Error(`translate-appearance: ${message}`);
}

async function state(client: ControlClient, action = ""): Promise<State> {
  const reply = await client.request(ControlCommand.TestTranslatePopup, action ? [action] : []);
  assert(reply[0] === 0, "popup diagnostic failed");
  const fields = Object.fromEntries(
    String(reply[1] ?? "")
      .split(/\r?\n/)
      .flatMap((line) => {
        const equal = line.indexOf("=");
        return equal < 0 ? [] : [[line.slice(0, equal), line.slice(equal + 1)]];
      }),
  );
  return {
    visible: fields.visible === "1",
    busy: fields.busy === "1",
    error: fields.error === "1",
    text: JSON.parse(fields.text || '""'),
    status: JSON.parse(fields.status || '""'),
    request: Number(fields.request || 0),
    fontSize: Number(fields.fontSize),
    fontPixels: Number(fields.fontPixels),
    autoSize: fields.autoSize === "1",
    preferredWidth: Number(fields.preferredWidth),
    preferredHeight: Number(fields.preferredHeight),
    dpi: Number(fields.dpi),
    resizeCount: Number(fields.resizeCount),
  };
}

async function waitState(session: Session, predicate: (value: State) => boolean, error: string): Promise<State> {
  return pollUntil(() => state(session.client), predicate, { timeoutMs: TIMEOUT, intervalMs: 10, error });
}

async function expectText(session: Session, text = SHORT): Promise<State> {
  const value = await waitState(
    session,
    (current) => current.visible && !current.busy && current.text === text,
    "translation did not finish with expected text",
  );
  assert(!value.error, "valid translation was marked as failed");
  return value;
}

async function drag(session: Session, word = "quick"): Promise<void> {
  const reply = await session.client.request(ControlCommand.TestRotatedTextMouseDrag, [word]);
  assert(String(reply[1] ?? "").includes(`selected=${word}\n`), "native selection did not select requested word");
}

function popup(session: Session): number {
  const hwnd = findTopWindow(session.proc.pid!, "SumatraTranslatePopup");
  assert(hwnd && isWindowVisible(hwnd), "native translation popup is not visible");
  return hwnd;
}

function body(session: Session): number {
  let edit = 0;
  enumChildWindows(popup(session), (child) => {
    if (getClassName(child).toLowerCase() === "edit") edit = child;
    return true;
  });
  assert(edit, "translation body EDIT is missing");
  return edit;
}

function dimensions(hwnd: number): { width: number; height: number } {
  const rect = getWindowRect(hwnd);
  return { width: rect.right - rect.left, height: rect.bottom - rect.top };
}

function insideWorkArea(hwnd: number): void {
  const rect = getWindowRect(hwnd);
  const area = getWorkArea();
  assert(
    rect.left >= area.left && rect.top >= area.top && rect.right <= area.right && rect.bottom <= area.bottom,
    "popup extends outside the work area",
  );
}

function assertChrome(session: Session): void {
  const hwnd = popup(session);
  const origin = clientToScreen(hwnd, 0, 0);
  const client = getClientRect(hwnd);
  const ids = [3001, 3002, 3003, 3004, 3005, ...Object.values(ReadingControl)];
  const rectangles = ids.map((id) => getWindowRect(controlById(hwnd, id)));
  for (const rect of rectangles) {
    assert(rect.right > rect.left && rect.bottom > rect.top, "popup button has no size");
    assert(
      rect.left >= origin.x &&
        rect.top >= origin.y &&
        rect.right <= origin.x + client.right &&
        rect.bottom <= origin.y + client.bottom,
      "popup button is outside its client area",
    );
  }
  rectangles.forEach((a, index) => {
    for (const b of rectangles.slice(index + 1)) {
      assert(
        a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top,
        "popup controls overlap",
      );
    }
  });
  const text = getWindowRect(body(session));
  assert(text.bottom <= rectangles[0]!.top && text.bottom > text.top, "translation body overlaps footer");
  for (const id of Object.values(ReadingControl)) {
    assert(getWindowRect(controlById(hwnd, id)).bottom <= text.top, "reading controls overlap translation body");
  }
  insideWorkArea(hwnd);
}

async function capture(session: Session, name: string): Promise<void> {
  const hwnd = popup(session);
  await pollUntil(
    () => {
      repaintWindow(hwnd);
      enumChildWindows(hwnd, (child) => {
        repaintWindow(child);
        return true;
      });
      return captureWindowToPng(hwnd, tmpPath(`translate-appearance-${name}.png`));
    },
    (ok) => ok,
    { timeoutMs: TIMEOUT, error: "could not capture popup" },
  );
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

function checkBox(hwnd: number, checked: boolean): void {
  if ((sendMessage(hwnd, BM_GETCHECK, 0, 0) === 1n) !== checked) sendMessage(hwnd, BM_CLICK, 0, 0);
}

async function nativeResize(session: Session, width: number, height: number): Promise<void> {
  const before = await state(session.client);
  const hwnd = popup(session);
  const rect = getWindowRect(hwnd);
  sendMessage(hwnd, WM_ENTERSIZEMOVE, 0, 0);
  try {
    assert(
      setWindowPos(
        hwnd,
        rect.left,
        rect.top,
        Math.round((width * before.dpi) / 96),
        Math.round((height * before.dpi) / 96),
      ),
      "native sizing operation failed",
    );
  } finally {
    sendMessage(hwnd, WM_EXITSIZEMOVE, 0, 0);
  }
  await waitState(
    session,
    (value) => !value.autoSize && value.preferredWidth === width && value.preferredHeight === height,
    "manual sizing was not saved in logical units",
  );
}

function sameSize(a: { width: number; height: number }, b: { width: number; height: number }): boolean {
  return a.width === b.width && a.height === b.height;
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

function chat(text = SHORT): Response {
  return Response.json({ choices: [{ message: { content: text }, finish_reason: "stop" }] });
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

function gatedChat(parts: { text: string; wait: Promise<void> }[], finish: Promise<void>): Response {
  let canceled = false;
  return new Response(
    new ReadableStream<Uint8Array>({
      async start(controller) {
        try {
          for (const part of parts) {
            await part.wait;
            if (canceled) return;
            controller.enqueue(Buffer.from(delta(part.text)));
          }
          await finish;
          if (canceled) return;
          controller.enqueue(Buffer.from(delta("", "stop")));
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
  );
}

async function holdManualSize(session: Session, expected: { width: number; height: number }): Promise<void> {
  const until = Date.now() + 300;
  while (Date.now() < until) {
    const current = await state(session.client);
    assert(
      current.busy && !current.autoSize && sameSize(dimensions(popup(session)), expected),
      "pending stream layout overwrote the manual window size",
    );
    await Bun.sleep(15);
  }
}

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const pdf = tmpPath("translate-appearance.pdf");
  writeFileSync(pdf, makeTextPdf("The quick brown fox reads a book."));
  const routes = new Map<string, { received: Request[]; reply: Reply }>();
  const results: { name: string; status: string; elapsedMs: number; reason?: string }[] = [];
  let ordinal = 0;
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    async fetch(request) {
      const route = routes.get(new URL(request.url).pathname);
      if (!route) return new Response("unknown appearance route", { status: 404 });
      const item = { body: await request.json(), url: request.url };
      route.received.push(item);
      return route.reply(item, route.received.length);
    },
  });

  async function run(
    name: string,
    check: (context: Context) => Promise<void>,
    options: { configured?: boolean; settings?: string[]; reply?: Reply; saveSettings?: boolean } = {},
  ): Promise<void> {
    if (process.env.SUMATRA_APPEARANCE_CASE && process.env.SUMATRA_APPEARANCE_CASE !== name) return;
    const started = Date.now();
    const path = `/${++ordinal}-${name}`;
    const endpoint = `${server.url.origin}${path}`;
    const received: Request[] = [];
    routes.set(path, { received, reply: options.reply ?? (() => chat()) });
    const configured = options.configured !== false;
    const appdata = writeAppdata(
      `translate-appearance-${ordinal}`,
      [
        "RestoreSession = false",
        "CheckForUpdates = false",
        "ShowToc = false",
        "SelectionToolbar = false",
        "TranslatePopupEnabled = true",
        `TranslatePopupUrl = ${configured ? endpoint : ""}`,
        `TranslatePopupModel = ${configured ? MODEL : ""}`,
        `TranslatePopupApiKey = ${configured ? KEY : ""}`,
        ...(options.settings ?? []),
      ].join("\n"),
    );
    const session = await launchControlled(["-appdata", appdata, pdf], { saveSettings: options.saveSettings });
    try {
      await session.client.waitForRenderIdle();
      await session.client.setNotificationsEnabled(false);
      await drag(session);
      await check({ session, received, endpoint, appdata });
      await quit(session);
      results.push({ name, status: "PASS", elapsedMs: Date.now() - started });
      console.log(`PASS: translate-appearance ${name}`);
    } catch (error) {
      results.push({ name, status: "FAIL", elapsedMs: Date.now() - started, reason: String(error) });
      console.error(`FAIL: translate-appearance ${name}: ${String(error)}`);
      for (const cls of ["SumatraTranslatePopup", "SumatraTranslateConfig"]) {
        const hwnd = findTopWindow(session.proc.pid!, cls);
        if (hwnd) captureWindowToPng(hwnd, tmpPath(`translate-appearance-${name}-${cls}.png`));
      }
      session.client.close();
      await killAndWait(session.proc);
    } finally {
      routes.delete(path);
      writeFileSync(tmpPath("translate-appearance-results.json"), JSON.stringify(results, null, 2));
    }
  }

  try {
    await run(
      "defaults",
      async ({ session, received }) => {
        const hwnd = popup(session);
        for (const id of Object.values(ReadingControl)) controlById(hwnd, id);
        const current = await state(session.client);
        assert(current.fontSize === 11 && current.fontPixels > 0 && current.autoSize, "reading defaults are wrong");
        assert(
          current.preferredWidth === 460 && current.preferredHeight === 350 && current.dpi >= 96,
          "logical size defaults are wrong",
        );
        const labels = [3001, 3002, 3003, 3004, 3005].map((id) => getControlText(controlById(hwnd, id)));
        assert(labels.join("|") === "复制|重试|固定|设置|关闭", "footer translation actions changed");
        let buttonCount = 0;
        enumChildWindows(hwnd, (child) => {
          if (getClassName(child).toLowerCase() === "button") buttonCount++;
          return true;
        });
        assert(buttonCount === 9, "unexpected translation popup controls");
        assertChrome(session);
        const form = await openConfig(session);
        assert(
          getControlText(form.field("font")) === "Microsoft YaHei UI" &&
            getControlText(form.field("fontSize")) === "11",
          "font settings defaults are wrong",
        );
        assert(
          sendMessage(form.field("autoSize"), BM_GETCHECK, 0, 0) === 1n &&
            getControlText(form.field("width")) === "460" &&
            getControlText(form.field("height")) === "350" &&
            getControlText(form.field("maxHeight")) === "70",
          "size settings defaults are wrong",
        );
        await closeConfig(form, false);
        assert(received.length === 0, "default setup sent an API request");
        await capture(session, "defaults");
      },
      { configured: false },
    );

    await run("quick-font-and-shortcuts", async ({ session, received }) => {
      const before = await expectText(session);
      const hwnd = popup(session);
      sendMessage(controlById(hwnd, ReadingControl.larger), BM_CLICK, 0, 0);
      const larger = await waitState(session, (value) => value.fontSize === 12, "A+ did not change font size");
      assert(larger.fontPixels > before.fontPixels, "A+ did not change actual font height");
      sendMessage(body(session), WM_MOUSEWHEEL, (120 << 16) | 8, 0);
      await waitState(session, (value) => value.fontSize === 13, "Ctrl+wheel did not increase font size");
      sendMessage(body(session), WM_MOUSEWHEEL, (-120 << 16) | 8, 0);
      await waitState(session, (value) => value.fontSize === 12, "Ctrl+wheel did not decrease font size");
      for (let i = 0; i < 40; i++) sendMessage(controlById(hwnd, ReadingControl.larger), BM_CLICK, 0, 0);
      const maximum = await waitState(session, (value) => value.fontSize === 32, "maximum font size is not clamped");
      for (let i = 0; i < 50; i++) sendMessage(controlById(hwnd, ReadingControl.smaller), BM_CLICK, 0, 0);
      const minimum = await waitState(session, (value) => value.fontSize === 8, "minimum font size is not clamped");
      assert(maximum.fontPixels > minimum.fontPixels, "font bounds did not affect rendered font");
      const cleanup = postKeyWithModifier(body(session), 0x30, VK_CONTROL);
      try {
        await waitState(session, (value) => value.fontSize === 11, "Ctrl+0 did not reset font size");
      } finally {
        cleanup();
      }
      const after = await state(session.client);
      assert(
        after.request === before.request && after.text === before.text && received.length === 1,
        "font changes changed translation or sent another request",
      );
      assertChrome(session);
      await capture(session, "quick-font");
    });

    await run(
      "automatic-short-long-short",
      async ({ session, received }) => {
        await expectText(session);
        const hwnd = popup(session);
        const short = dimensions(hwnd);
        await state(session.client, "retry");
        const longState = await expectText(session, LONG);
        const long = dimensions(hwnd);
        const area = getClientRect(findCanvas(session.frame));
        assert(
          long.width === short.width && long.height > short.height + longState.fontPixels * 2,
          "long translation did not grow height at stable reading width",
        );
        assert(
          long.height <= Math.max((180 * longState.dpi) / 96, Math.floor((area.bottom - area.top) * 0.7)) + 2,
          "automatic popup exceeded configured reading-area height limit",
        );
        const edit = body(session);
        assert(Number(sendMessage(edit, EM_GETLINECOUNT, 0, 0)) > 30, "long translation was not laid out");
        sendMessage(edit, EM_LINESCROLL, 0, 100000);
        assert(Number(sendMessage(edit, EM_GETFIRSTVISIBLELINE, 0, 0)) > 0, "long translation cannot scroll");
        sendMessage(edit, WM_MOUSEWHEEL, -120 << 16, 0);
        assert((await state(session.client)).fontSize === 11, "plain wheel changed font size");
        assertChrome(session);
        await capture(session, "long-auto");
        await state(session.client, "retry");
        await expectText(session);
        assert(sameSize(dimensions(hwnd), short), "short translation did not shrink back to fitted height");
        assert(received.length === 3, "automatic sizing triggered extra requests");
      },
      { reply: (_request, ordinal) => chat(ordinal === 2 ? LONG : SHORT) },
    );

    const chunks = Array.from(
      { length: 16 },
      (_, index) => `第${index + 1}段：` + "流式输出用于检查逐步增长和稳定阅读宽度。".repeat(3) + "\n",
    );
    const emissionStart = gate();
    const finish = gate();
    const emissionTimes: number[] = [];
    try {
      await run(
        "streaming-growth-throttle",
        async ({ session, received }) => {
          await waitState(session, (value) => value.busy && !value.text, "stream did not start");
          const hwnd = popup(session);
          sendMessage(controlById(hwnd, ReadingControl.larger), BM_CLICK, 0, 0);
          await waitState(
            session,
            (value) => value.busy && value.fontSize === 12,
            "busy font adjustment canceled the stream",
          );
          sendMessage(controlById(hwnd, ReadingControl.smaller), BM_CLICK, 0, 0);
          const before = await waitState(
            session,
            (value) => value.busy && value.fontSize === 11,
            "busy font reset failed",
          );
          const width = dimensions(hwnd).width;
          const initialHeight = dimensions(hwnd).height;
          const started = Date.now();
          const text = chunks.join("");
          let previousHeight = initialHeight;
          const sampleStream = async () => {
            const current = await state(session.client);
            const size = dimensions(hwnd);
            assert(current.busy, "stream finished before its terminal event was released");
            assert(size.width === width && size.height >= previousHeight, "streaming resized width or shrank height");
            previousHeight = size.height;
            return current;
          };
          emissionStart.release();
          let firstGrowthAfterChunk = 0;
          await pollUntil(
            sampleStream,
            (value) => {
              const grew =
                value.text.length > 0 &&
                value.resizeCount > before.resizeCount &&
                dimensions(hwnd).height > initialHeight;
              assert(
                emissionTimes.length < chunks.length,
                "automatic height only grew after continuous chunk emission stopped",
              );
              if (grew) firstGrowthAfterChunk = emissionTimes.length;
              return grew;
            },
            {
              timeoutMs: TIMEOUT,
              intervalMs: 10,
              error: "automatic height did not grow during ongoing continuous emission",
            },
          );
          const firstGrowthElapsed = Date.now() - started;
          const grown = await pollUntil(
            sampleStream,
            (value) => value.text === text && emissionTimes.length === chunks.length,
            { timeoutMs: TIMEOUT, intervalMs: 10, error: "continuous stream did not deliver all chunks" },
          );
          const tokenElapsed = emissionTimes[emissionTimes.length - 1]! - emissionTimes[0]!;
          assert(tokenElapsed >= 600, "continuous stream was too short to distinguish throttle from debounce");
          const changes = grown.resizeCount - before.resizeCount;
          const allowed = Math.ceil(tokenElapsed / 150) + 2;
          assert(changes <= allowed, "stream resized on too many individual text updates");
          assert(dimensions(hwnd).height > (180 * grown.dpi) / 96, "stream did not visibly grow");
          assert(received.length === 1 && grown.request === before.request, "busy font/sizing restarted API request");
          assertChrome(session);
          await capture(session, "streaming");
          finish.release();
          await expectText(session, text);
          writeFileSync(
            tmpPath("translate-appearance-streaming.json"),
            JSON.stringify(
              {
                chunks: chunks.length,
                tokenElapsedMs: tokenElapsed,
                automaticSizeChanges: changes,
                allowed,
                scheduledChunkIntervalMs: 45,
                firstGrowthAfterChunk,
                firstGrowthElapsedMs: firstGrowthElapsed,
                heightGrewWhileMoreChunksWerePending:
                  firstGrowthAfterChunk > 0 && firstGrowthAfterChunk < chunks.length,
                maximumEmissionGapMs: Math.max(
                  ...emissionTimes.slice(1).map((time, index) => time - emissionTimes[index]!),
                ),
                firstChunksObservedBeforeFinish: true,
              },
              null,
              2,
            ),
          );
        },
        {
          settings: ["TranslatePopupMaxHeight = 90"],
          reply: () => {
            let canceled = false;
            return new Response(
              new ReadableStream<Uint8Array>({
                async start(controller) {
                  try {
                    await emissionStart.promise;
                    for (let i = 0; i < chunks.length; i++) {
                      if (canceled) return;
                      emissionTimes.push(Date.now());
                      controller.enqueue(Buffer.from(delta(chunks[i]!)));
                      if (i + 1 < chunks.length) await Bun.sleep(45);
                    }
                    await finish.promise;
                    if (canceled) return;
                    controller.enqueue(Buffer.from(delta("", "stop")));
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
            );
          },
        },
      );
    } finally {
      emissionStart.release();
      finish.release();
    }

    const exactFinish = gate();
    try {
      await run(
        "streaming-final-exact-fit",
        async ({ session, received }) => {
          const before = await waitState(
            session,
            (value) => value.busy && value.text === SHORT,
            "short stream text did not arrive before completion",
          );
          const hwnd = popup(session);
          const canvas = findCanvas(session.frame);
          const area = getClientRect(canvas);
          const origin = clientToScreen(canvas, area.left, area.top);
          const rect = getWindowRect(hwnd);
          const enlarged = area.bottom - area.top;
          assert(
            setWindowPos(hwnd, rect.left, origin.y, rect.right - rect.left, enlarged),
            "could not enlarge automatic popup before stream completion",
          );
          const held = await state(session.client);
          assert(
            held.busy && held.autoSize && held.preferredWidth === 460 && held.preferredHeight === 350,
            "programmatic enlargement changed mode or preferences",
          );
          exactFinish.release();
          const completed = await expectText(session);
          const fitted = dimensions(hwnd);
          const minimum = Math.min(Math.round((180 * completed.dpi) / 96), enlarged);
          assert(
            fitted.height < enlarged && Math.abs(fitted.height - minimum) <= 2,
            "stream completion did not refit short text to minimum reading height",
          );
          assert(
            completed.request === before.request && received.length === 1,
            "final fitting restarted the translation request",
          );
          assertChrome(session);
          await capture(session, "stream-final-fit");
          writeFileSync(
            tmpPath("translate-appearance-final-fit.json"),
            JSON.stringify(
              {
                heldOpen: true,
                enlargedHeight: enlarged,
                finalHeight: fitted.height,
                expectedMinimum: minimum,
                requestUnchanged: true,
              },
              null,
              2,
            ),
          );
        },
        { reply: () => gatedChat([{ text: SHORT, wait: Promise.resolve() }], exactFinish.promise) },
      );
    } finally {
      exactFinish.release();
    }

    const manualLater = gate();
    const manualFinish = gate();
    try {
      await run(
        "manual-resize-during-stream",
        async ({ session, received }) => {
          const before = await waitState(
            session,
            (value) => value.busy && value.text === SHORT,
            "manual stream did not expose its first chunk",
          );
          await nativeResize(session, 440, 310);
          const hwnd = popup(session);
          const manual = dimensions(hwnd);
          sendMessage(controlById(hwnd, ReadingControl.larger), BM_CLICK, 0, 0);
          await waitState(
            session,
            (value) => value.busy && value.fontSize === 12 && !value.autoSize,
            "busy font adjustment changed manual mode or canceled stream",
          );
          assert(sameSize(dimensions(hwnd), manual), "busy font change overwrote manual dimensions");
          manualLater.release();
          await waitState(
            session,
            (value) => value.busy && value.text === SHORT + LONG,
            "later long chunk was not visible while stream remained open",
          );
          await holdManualSize(session, manual);
          manualFinish.release();
          const completed = await expectText(session, SHORT + LONG);
          assert(
            !completed.autoSize && sameSize(dimensions(hwnd), manual),
            "final stream result overwrote manually selected size",
          );
          assert(
            completed.request === before.request && received.length === 1,
            "manual resize or busy font change sent an API request",
          );
          assertChrome(session);
          await capture(session, "manual-during-stream");
          writeFileSync(
            tmpPath("translate-appearance-manual-stream.json"),
            JSON.stringify(
              {
                firstChunkObservedBeforeResize: true,
                laterLongChunkObservedBeforeFinish: true,
                manualWidth: manual.width,
                manualHeight: manual.height,
                stableThroughPendingLayoutMs: 300,
                finalSizeUnchanged: true,
                requestUnchanged: true,
              },
              null,
              2,
            ),
          );
        },
        {
          reply: () =>
            gatedChat(
              [
                { text: SHORT, wait: Promise.resolve() },
                { text: LONG, wait: manualLater.promise },
              ],
              manualFinish.promise,
            ),
        },
      );
    } finally {
      manualLater.release();
      manualFinish.release();
    }

    await run(
      "manual-size-and-restore-auto",
      async ({ session, received }) => {
        await expectText(session);
        const hwnd = popup(session);
        const before = await state(session.client);
        const rect = getWindowRect(hwnd);
        assert(
          setWindowPos(
            hwnd,
            rect.left,
            rect.top,
            Math.round((500 * before.dpi) / 96),
            Math.round((360 * before.dpi) / 96),
          ),
          "programmatic size probe failed",
        );
        const programmatic = await state(session.client);
        assert(
          programmatic.autoSize && programmatic.preferredWidth === 460 && programmatic.preferredHeight === 350,
          "programmatic sizing was incorrectly persisted as a manual drag",
        );
        await nativeResize(session, 430, 300);
        const manual = dimensions(hwnd);
        sendMessage(controlById(hwnd, ReadingControl.larger), BM_CLICK, 0, 0);
        await waitState(session, (value) => value.fontSize === 12, "manual font adjustment failed");
        assert(sameSize(dimensions(hwnd), manual), "font adjustment overwrote manual dimensions");
        await state(session.client, "retry");
        await expectText(session, LONG);
        assert(sameSize(dimensions(hwnd), manual), "translation result overwrote manual dimensions");
        await drag(session, "brown");
        await expectText(session);
        assert(sameSize(dimensions(hwnd), manual), "new selection overwrote manual dimensions");
        const count = received.length;
        const request = (await state(session.client)).request;
        sendMessage(controlById(hwnd, ReadingControl.autoSize), BM_CLICK, 0, 0);
        await waitState(session, (value) => value.autoSize, "self-adapting mode did not restore");
        assert(dimensions(hwnd).height < manual.height, "restoring automatic size did not fit short translation");
        assert(
          received.length === count && (await state(session.client)).request === request,
          "auto-size toggle sent a request",
        );
        assertChrome(session);
        await capture(session, "restored-auto");
      },
      { reply: (_request, ordinal) => chat(ordinal === 2 ? LONG : SHORT) },
    );

    await run("appearance-save-and-cancel", async ({ session, received }) => {
      const before = await expectText(session);
      let form = await openConfig(session);
      selectCombo(form.field("font"), /^Segoe UI$/i);
      sendText(form.field("fontSize"), "18");
      sendText(form.field("width"), "520");
      sendText(form.field("maxHeight"), "60");
      await closeConfig(form, true);
      const applied = await waitState(
        session,
        (value) => value.fontSize === 18 && value.preferredWidth === 520,
        "display-only settings were not applied",
      );
      assert(
        applied.fontPixels > before.fontPixels &&
          applied.text === before.text &&
          applied.request === before.request &&
          received.length === 1,
        "display-only save changed default prompt semantics or retried translation",
      );
      form = await openConfig(session);
      assert(
        getControlText(form.field("font")) === "Segoe UI" && getControlText(form.field("maxHeight")) === "60",
        "font family or automatic height preference was not saved",
      );
      sendText(form.field("fontSize"), "26");
      sendText(form.field("width"), "560");
      checkBox(form.field("autoSize"), false);
      sendText(form.field("height"), "320");
      await closeConfig(form, false);
      const canceled = await state(session.client);
      assert(
        canceled.fontSize === 18 &&
          canceled.autoSize &&
          canceled.preferredWidth === 520 &&
          canceled.request === before.request &&
          received.length === 1,
        "Cancel applied display draft or sent request",
      );
      assertChrome(session);
      await capture(session, "configured-font");
    });

    await run("font-combo-keyboard", async ({ session, received }) => {
      const before = await expectText(session);
      const form = await openConfig(session);
      const combo = form.field("font");
      const fontSize = form.field("fontSize");
      let edit = 0;
      enumChildWindows(combo, (child) => {
        if (getClassName(child).toLowerCase() === "edit") edit = child;
        return true;
      });
      assert(edit, "font combo internal EDIT is missing");
      assert(getFocusedHwnd(form.hwnd) === form.field("url"), "settings did not initially focus URL");
      sendText(fontSize, "19");
      const tabToFontEdit = () => {
        for (let index = 0; index < 40; index++) {
          const focused = getFocusedHwnd(form.hwnd);
          if (focused === edit) return;
          assert(focused, "settings lost keyboard focus while tabbing to font combo");
          sendMessage(focused, WM_KEYDOWN, VK_TAB, 0);
        }
        throw new Error("translate-appearance: Tab traversal did not reach font combo internal EDIT");
      };
      tabToFontEdit();
      sendMessage(edit, WM_KEYDOWN, VK_RETURN, 0);
      assert(isWindowVisible(form.hwnd), "Enter in font combo saved the settings instead of accepting input");
      assert((await state(session.client)).fontSize === before.fontSize, "Enter applied the font draft");
      sendMessage(edit, WM_KEYDOWN, VK_TAB, 0);
      assert(getFocusedHwnd(form.hwnd) === fontSize, "Tab from font combo internal EDIT did not reach font size");
      tabToFontEdit();
      sendMessage(edit, WM_KEYDOWN, VK_ESCAPE, 0);
      await pollUntil(
        () => isWindowVisible(form.hwnd),
        (visible) => !visible,
        { timeoutMs: TIMEOUT, error: "Escape in font combo internal EDIT did not cancel settings" },
      );
      const after = await state(session.client);
      assert(
        after.fontSize === before.fontSize &&
          after.text === before.text &&
          after.request === before.request &&
          received.length === 1,
        "font combo keyboard cancellation applied draft or changed translation request",
      );
      writeFileSync(
        tmpPath("translate-appearance-font-combo.json"),
        JSON.stringify(
          {
            tabFromUrlReachedComboEdit: true,
            enterKeptDraft: true,
            tabReachedFontSize: true,
            escapeCanceledDraft: true,
            fontUnchanged: true,
            requestUnchanged: true,
          },
          null,
          2,
        ),
      );
    });

    await run(
      "appearance-without-provider",
      async ({ session, received }) => {
        const before = await state(session.client);
        assert(before.status.includes("请先设置翻译服务"), "unconfigured popup did not show setup prompt");
        const form = await openConfig(session);
        sendText(form.field("fontSize"), "16");
        sendText(form.field("width"), "500");
        await closeConfig(form, true);
        const after = await waitState(
          session,
          (value) => value.fontSize === 16 && value.preferredWidth === 500,
          "display settings could not be saved without provider",
        );
        assert(
          after.status === before.status &&
            after.text === before.text &&
            after.request === before.request &&
            received.length === 0,
          "unconfigured appearance save changed setup state or sent request",
        );
      },
      { configured: false },
    );

    await run("invalid-display-fields", async ({ session, received }) => {
      const before = await expectText(session);
      const form = await openConfig(session);
      for (const [name, values, valid] of [
        ["fontSize", ["7", "33", "11oops"], "11"],
        ["width", ["329", "1601"], "460"],
        ["maxHeight", ["29", "91"], "70"],
      ] as [keyof typeof TranslateControl, string[], string][]) {
        for (const value of values) {
          sendText(form.field(name), value);
          sendMessage(form.field("save"), BM_CLICK, 0, 0);
          assert(isWindowVisible(form.hwnd), `invalid ${name} ${value} was accepted`);
        }
        sendText(form.field(name), valid);
      }
      checkBox(form.field("autoSize"), false);
      for (const value of ["179", "1201"]) {
        sendText(form.field("height"), value);
        sendMessage(form.field("save"), BM_CLICK, 0, 0);
        assert(isWindowVisible(form.hwnd), "invalid manual height was accepted");
      }
      await closeConfig(form, false);
      assert(
        (await state(session.client)).request === before.request && received.length === 1,
        "invalid display settings changed request",
      );
    });

    await run(
      "persist-font-and-manual-size",
      async ({ session, received, appdata }) => {
        await expectText(session);
        const form = await openConfig(session);
        selectCombo(form.field("font"), /^Segoe UI$/i);
        sendText(form.field("fontSize"), "15");
        sendText(form.field("maxHeight"), "65");
        await closeConfig(form, true);
        await nativeResize(session, 440, 310);
        const settingsPath = join(appdata, "SumatraPDF-settings.txt");
        await pollUntil(
          () => (existsSync(settingsPath) ? readFileSync(settingsPath, "utf8") : ""),
          (text) =>
            /TranslatePopupFontSize\s*=\s*15\b/.test(text) &&
            /TranslatePopupAutoSize\s*=\s*false\b/.test(text) &&
            /TranslatePopupWidth\s*=\s*440\b/.test(text) &&
            /TranslatePopupHeight\s*=\s*310\b/.test(text) &&
            /TranslatePopupFont\s*=\s*Segoe UI\s*$/m.test(text) &&
            /TranslatePopupMaxHeight\s*=\s*65\b/.test(text),
          { timeoutMs: TIMEOUT, error: "reading preferences were not persisted" },
        );
        await quit(session);
        const count = received.length;
        const restarted = await launchControlled(["-appdata", appdata, pdf]);
        try {
          await restarted.client.waitForRenderIdle();
          await restarted.client.setNotificationsEnabled(false);
          await drag(restarted);
          const reloaded = await expectText(restarted);
          assert(
            reloaded.fontSize === 15 &&
              !reloaded.autoSize &&
              reloaded.preferredWidth === 440 &&
              reloaded.preferredHeight === 310,
            "reading preferences did not reload",
          );
          const size = dimensions(popup(restarted));
          const area = getClientRect(findCanvas(restarted.frame));
          assert(
            Math.abs(size.width - Math.min((440 * reloaded.dpi) / 96, area.right - area.left)) <= 2 &&
              Math.abs(size.height - Math.min((310 * reloaded.dpi) / 96, area.bottom - area.top)) <= 2,
            "reloaded DIP size did not scale to current DPI",
          );
          assert(received.length === count + 1, "reload triggered extra translation requests");
          const reloadedForm = await openConfig(restarted);
          assert(
            getControlText(reloadedForm.field("font")) === "Segoe UI" &&
              getControlText(reloadedForm.field("maxHeight")) === "65",
            "font family or automatic height limit failed to reload",
          );
          await closeConfig(reloadedForm, false);
          assertChrome(restarted);
          await capture(restarted, "persisted-manual");
          writeFileSync(
            tmpPath("translate-appearance-persistence.json"),
            JSON.stringify(
              {
                fontFamily: true,
                fontSize: true,
                manualMode: true,
                logicalWidth: true,
                logicalHeight: true,
                heightLimit: true,
                actualDpi: reloaded.dpi,
                observedWidth: size.width,
                observedHeight: size.height,
              },
              null,
              2,
            ),
          );
        } finally {
          await quit(restarted);
        }
      },
      { saveSettings: true },
    );

    await run(
      "clamped-manual-preferences-survive",
      async ({ session, received, appdata }) => {
        const before = await expectText(session);
        const area = getClientRect(findCanvas(session.frame));
        const desiredWidth = Math.round((1600 * before.dpi) / 96);
        const desiredHeight = Math.round((1200 * before.dpi) / 96);
        const forced = desiredWidth > area.right - area.left || desiredHeight > area.bottom - area.top;
        const assertPreferences = async (currentSession: Session) => {
          const current = await state(currentSession.client);
          const view = getClientRect(findCanvas(currentSession.frame));
          assert(
            !current.autoSize && current.preferredWidth === 1600 && current.preferredHeight === 1200,
            "reading-area clamp overwrote remembered logical dimensions",
          );
          const size = dimensions(popup(currentSession));
          assert(
            Math.abs(size.width - Math.min((1600 * current.dpi) / 96, view.right - view.left)) <= 2 &&
              Math.abs(size.height - Math.min((1200 * current.dpi) / 96, view.bottom - view.top)) <= 2,
            "manual window was not clamped to current reading area",
          );
          insideWorkArea(popup(currentSession));
        };
        await assertPreferences(session);
        sendMessage(controlById(popup(session), ReadingControl.larger), BM_CLICK, 0, 0);
        await waitState(session, (value) => value.fontSize === 12, "clamped font change did not apply");
        await assertPreferences(session);
        const form = await openConfig(session);
        assert(
          getControlText(form.field("width")) === "1600" && getControlText(form.field("height")) === "1200",
          "settings displayed temporary clamped dimensions as preferences",
        );
        sendText(form.field("fontSize"), "14");
        await closeConfig(form, true);
        await waitState(session, (value) => value.fontSize === 14, "clamped display save failed");
        await assertPreferences(session);
        assert(
          received.length === 1 && (await state(session.client)).request === before.request,
          "clamped font changes sent a translation request",
        );
        const settingsPath = join(appdata, "SumatraPDF-settings.txt");
        await pollUntil(
          () => (existsSync(settingsPath) ? readFileSync(settingsPath, "utf8") : ""),
          (text) =>
            /TranslatePopupWidth\s*=\s*1600\b/.test(text) &&
            /TranslatePopupHeight\s*=\s*1200\b/.test(text) &&
            /TranslatePopupFontSize\s*=\s*14\b/.test(text) &&
            /TranslatePopupAutoSize\s*=\s*false\b/.test(text),
          { timeoutMs: TIMEOUT, error: "unclamped logical preferences were not persisted" },
        );
        await quit(session);
        const count = received.length;
        const restarted = await launchControlled(["-appdata", appdata, pdf]);
        try {
          await restarted.client.waitForRenderIdle();
          await restarted.client.setNotificationsEnabled(false);
          await drag(restarted);
          const reloaded = await expectText(restarted);
          await assertPreferences(restarted);
          assert(
            reloaded.fontSize === 14 && received.length === count + 1,
            "clamped preferences failed to reload without extra requests",
          );
          assertChrome(restarted);
          await capture(restarted, "clamped-manual");
          const status = forced ? "PASS" : "SKIP";
          const reason = forced
            ? "reading area is smaller than requested manual dimensions"
            : "reading area is too large to force a clamp at supported maximum preferences";
          if (!forced) console.log(`SKIP: translate-appearance forced-reading-area-clamp: ${reason}`);
          writeFileSync(
            tmpPath("translate-appearance-clamped-preferences.json"),
            JSON.stringify(
              {
                forcedClamp: { status, reason },
                preferencePersistence: "PASS",
                fontChangePreservedPreferences: true,
                displaySavePreservedPreferences: true,
                restartPreservedPreferences: true,
                preferredWidth: 1600,
                preferredHeight: 1200,
                canvasWidth: area.right - area.left,
                canvasHeight: area.bottom - area.top,
                dpi: reloaded.dpi,
              },
              null,
              2,
            ),
          );
        } finally {
          await quit(restarted);
        }
      },
      {
        saveSettings: true,
        settings: ["TranslatePopupAutoSize = false", "TranslatePopupWidth = 1600", "TranslatePopupHeight = 1200"],
      },
    );

    await run(
      "large-font-small-manual-window",
      async ({ session, received }) => {
        const before = await expectText(session, LONG);
        assert(before.fontSize === 32 && !before.autoSize, "configured large font/manual mode did not load");
        assertChrome(session);
        const hwnd = popup(session);
        const size = dimensions(hwnd);
        assert(
          Math.abs(size.width - (330 * before.dpi) / 96) <= 2 && Math.abs(size.height - (180 * before.dpi) / 96) <= 2,
          "minimum manual size did not load",
        );
        sendMessage(body(session), EM_LINESCROLL, 0, 100000);
        assert(
          Number(sendMessage(body(session), EM_GETFIRSTVISIBLELINE, 0, 0)) > 0,
          "large-font translation cannot scroll",
        );
        sendMessage(controlById(hwnd, ReadingControl.smaller), BM_CLICK, 0, 0);
        await waitState(session, (value) => value.fontSize === 31, "large-font decrement failed");
        assert(
          sameSize(dimensions(hwnd), size) && received.length === 1,
          "font adjustment resized manual popup or requested again",
        );
        await capture(session, "large-font-small-window");
      },
      {
        settings: [
          "TranslatePopupFontSize = 32",
          "TranslatePopupAutoSize = false",
          "TranslatePopupWidth = 330",
          "TranslatePopupHeight = 180",
        ],
        reply: () => chat(LONG),
      },
    );
  } finally {
    server.stop(true);
  }
  assert(results.length > 0, "no appearance scenario selected");
  assert(
    results.every((item) => item.status === "PASS"),
    `${results.filter((item) => item.status === "FAIL").length} appearance scenarios failed`,
  );
}

if (import.meta.main) await runStandalone(testit);
