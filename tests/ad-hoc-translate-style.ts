// SPDX-License-Identifier: GPL-3.0-only
// Run: bun tests/ad-hoc-translate-style.ts --no-build
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand } from "./control.ts";
import { loadPng, makeTextPdf, pollUntil, runStandalone, tmpPath, writeAppdata, type PngImage } from "./util.ts";
import { killAndWait, launchControlled } from "./win-automation.ts";
import {
  captureWindowToPng,
  clientToScreen,
  enumChildWindows,
  findTopWindow,
  getClientRect,
  getControlText,
  getWindowRect,
  getWorkArea,
  isWindowVisible,
  repaintWindow,
  sendMessage,
  sendText,
  setProcessDpiAware,
  setWindowPos,
} from "./winapi.ts";
import { controlById, ReadingControl, TranslateControl, translationBody } from "./translate-test-controls.ts";

const TIMEOUT = 8000;
const BM_CLICK = 0x00f5;
const BM_GETCHECK = 0x00f0;
const WM_KEYDOWN = 0x0100;
const VK_RETURN = 0x0d;
const WM_ENTERSIZEMOVE = 0x0231;
const WM_EXITSIZEMOVE = 0x0232;
const EM_SETSEL = 0x00b1;
const EM_GETSEL = 0x00b0;
const EM_GETLINECOUNT = 0x00ba;
const EM_GETFIRSTVISIBLELINE = 0x00ce;
const EM_LINESCROLL = 0x00b6;
const TEXT = "白色卡片与加粗显示测试：量子光学 α≤β，[12]。";
const LONG = (TEXT + "\n").repeat(28);
const MODEL = "style-test-model";
const KEY = "style-test-not-a-secret";
type Session = Awaited<ReturnType<typeof launchControlled>>;
type State = {
  visible: boolean;
  busy: boolean;
  error: boolean;
  pinned: boolean;
  text: string;
  status: string;
  request: number;
  fontSize: number;
  fontPixels: number;
  bold: boolean;
  fontWeight: number;
  autoSize: boolean;
  preferredWidth: number;
  preferredHeight: number;
  dpi: number;
};
type Form = { hwnd: number; field: (name: keyof typeof TranslateControl) => number };
type MockRequest = { url: string; model: string; stream: boolean };
type Context = { session: Session; received: MockRequest[]; endpoint: string; appdata: string; frameBand: number };
type Reply = () => Response;
type Rect = { left: number; top: number; right: number; bottom: number };

function assert(value: unknown, message: string): asserts value {
  if (!value) throw new Error(`translate-style: ${message}`);
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
    pinned: fields.pinned === "1",
    text: JSON.parse(fields.text || '""'),
    status: JSON.parse(fields.status || '""'),
    request: Number(fields.request || 0),
    fontSize: Number(fields.fontSize),
    fontPixels: Number(fields.fontPixels),
    bold: fields.bold === "1",
    fontWeight: Number(fields.fontWeight),
    autoSize: fields.autoSize === "1",
    preferredWidth: Number(fields.preferredWidth),
    preferredHeight: Number(fields.preferredHeight),
    dpi: Number(fields.dpi),
  };
}

async function waitState(session: Session, predicate: (value: State) => boolean, error: string): Promise<State> {
  return pollUntil(() => state(session.client), predicate, { timeoutMs: TIMEOUT, intervalMs: 15, error });
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

async function drag(session: Session): Promise<void> {
  const reply = await session.client.request(ControlCommand.TestRotatedTextMouseDrag, ["quick"]);
  assert(String(reply[1] ?? "").includes("selected=quick\n"), "native selection failed");
}

function popup(session: Session): number {
  const hwnd = findTopWindow(session.proc.pid!, "SumatraTranslatePopup");
  assert(hwnd && isWindowVisible(hwnd), "native translation popup is missing");
  return hwnd;
}

function body(session: Session): number {
  return translationBody(popup(session));
}

function dimensions(hwnd: number): { width: number; height: number } {
  const rect = getWindowRect(hwnd);
  return { width: rect.right - rect.left, height: rect.bottom - rect.top };
}

function sameSize(a: { width: number; height: number }, b: { width: number; height: number }): boolean {
  return a.width === b.width && a.height === b.height;
}

async function nativeResize(session: Session, width: number, height: number): Promise<void> {
  const current = await state(session.client);
  const hwnd = popup(session);
  const rect = getWindowRect(hwnd);
  sendMessage(hwnd, WM_ENTERSIZEMOVE, 0, 0);
  try {
    assert(
      setWindowPos(
        hwnd,
        rect.left,
        rect.top,
        Math.round((width * current.dpi) / 96),
        Math.round((height * current.dpi) / 96),
      ),
      "native sizing failed",
    );
  } finally {
    sendMessage(hwnd, WM_EXITSIZEMOVE, 0, 0);
  }
  await waitState(
    session,
    (value) => !value.autoSize && value.preferredWidth === width && value.preferredHeight === height,
    "manual dimensions did not apply",
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
    (visible) => !visible,
    {
      timeoutMs: TIMEOUT,
      error: "settings did not close",
    },
  );
}

function checkBox(hwnd: number, checked: boolean): void {
  if ((sendMessage(hwnd, BM_GETCHECK, 0, 0) === 1n) !== checked) sendMessage(hwnd, BM_CLICK, 0, 0);
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

function capture(hwnd: number, path: string): boolean {
  repaintWindow(hwnd);
  enumChildWindows(hwnd, (child) => {
    repaintWindow(child);
    return true;
  });
  return captureWindowToPng(hwnd, path);
}

function pixels(image: PngImage, rect: Rect): [number, number, number][] {
  const values: [number, number, number][] = [];
  for (let y = Math.max(0, Math.ceil(rect.top)); y < Math.min(image.h, Math.floor(rect.bottom)); y++) {
    for (let x = Math.max(0, Math.ceil(rect.left)); x < Math.min(image.w, Math.floor(rect.right)); x++) {
      const at = (y * image.w + x) * image.nComp;
      values.push([image.data[at]!, image.data[at + 1]!, image.data[at + 2]!]);
    }
  }
  assert(values.length > 0, "pixel probe sampled an empty region");
  return values;
}

function median(values: number[]): number {
  values.sort((a, b) => a - b);
  return values[Math.floor(values.length / 2)]!;
}

function frameThemeBand(session: Session, path: string): number {
  assert(capture(session.frame, path), "could not capture reader frame");
  const image = loadPng(path);
  return median(
    pixels(image, { left: image.w * 0.44, right: image.w * 0.56, top: 8, bottom: 20 }).map(
      ([r, g, b]) => (r + g + b) / 3,
    ),
  );
}

async function checkCard(session: Session, name: string): Promise<void> {
  const hwnd = popup(session);
  const current = await state(session.client);
  const scale = current.dpi / 96;
  const frame = getWindowRect(hwnd);
  const text = getWindowRect(body(session));
  const origin = clientToScreen(hwnd, 0, 0);
  const path = tmpPath(`translate-style-${name}.png`);
  await pollUntil(
    () => {
      if (!capture(hwnd, path)) return false;
      const image = loadPng(path);
      const blank = pixels(image, {
        left: text.left - frame.left + 4 * scale,
        right: text.right - frame.left - 26 * scale,
        top: text.bottom - frame.top - 15 * scale,
        bottom: text.bottom - frame.top - 5 * scale,
      });
      const whiteFraction = blank.filter(([r, g, b]) => r >= 250 && g >= 250 && b >= 250).length / blank.length;
      const ink = pixels(image, {
        left: text.left - frame.left + 2 * scale,
        right: text.right - frame.left - 26 * scale,
        top: text.top - frame.top + scale,
        bottom: Math.min(text.bottom - frame.top - 18 * scale, text.top - frame.top + 56 * scale),
      });
      const darkInkPixels = ink.filter(([r, g, b]) => r < 100 && g < 100 && b < 100).length;
      const middle = Math.round((text.top + text.bottom) / 2) - frame.top;
      const shell = pixels(image, {
        left: origin.x - frame.left + 2 * scale,
        right: origin.x - frame.left + 6 * scale,
        top: middle - 3 * scale,
        bottom: middle + 3 * scale,
      });
      const shellMedian = median(shell.map(([r, g, b]) => (r + g + b) / 3));
      const shellNeutral = shell.every(([r, g, b]) => Math.max(r, g, b) - Math.min(r, g, b) <= 3);
      const edge: number[] = [];
      for (let x = Math.ceil(text.left - frame.left) - 1; x > origin.x - frame.left + scale; x--) {
        const at = (middle * image.w + x) * image.nComp;
        edge.push((image.data[at]! + image.data[at + 1]! + image.data[at + 2]!) / 3);
      }
      const whitePadding = edge.findIndex((value) => value < 250);
      const borderPixels = edge.slice(Math.max(0, whitePadding), Math.max(0, whitePadding) + Math.ceil(3 * scale));
      const borderVisible = borderPixels.some((value) => value >= 150 && value <= 235);
      const receipt = {
        whiteFraction,
        darkInkPixels,
        shellMedian,
        shellNeutral,
        whitePaddingPixels: whitePadding,
        borderVisible,
        dpi: current.dpi,
      };
      writeFileSync(tmpPath(`translate-style-${name}-pixels.json`), JSON.stringify(receipt, null, 2));
      return (
        whiteFraction >= 0.98 &&
        darkInkPixels >= 40 &&
        shellNeutral &&
        shellMedian >= 235 &&
        shellMedian <= 248 &&
        whitePadding >= 2 * scale &&
        borderVisible
      );
    },
    (valid) => valid,
    { timeoutMs: TIMEOUT, intervalMs: 40, error: "white card, dark text, gray shell or card padding was not drawn" },
  );
}

function checkChrome(session: Session): void {
  const hwnd = popup(session);
  const origin = clientToScreen(hwnd, 0, 0);
  const client = getClientRect(hwnd);
  const ids = [3001, 3002, 3003, 3004, 3005, ...Object.values(ReadingControl)];
  const rects = ids.map((id) => getWindowRect(controlById(hwnd, id)));
  for (const rect of rects) {
    assert(
      rect.right > rect.left &&
        rect.bottom > rect.top &&
        rect.left >= origin.x &&
        rect.top >= origin.y &&
        rect.right <= origin.x + client.right &&
        rect.bottom <= origin.y + client.bottom,
      "popup button is outside its client area",
    );
  }
  rects.forEach((a, index) => {
    for (const b of rects.slice(index + 1)) {
      assert(
        a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top,
        "popup controls overlap",
      );
    }
  });
  const text = getWindowRect(body(session));
  assert(text.bottom > text.top && text.bottom <= rects[0]!.top, "translation body overlaps footer");
  for (const id of Object.values(ReadingControl)) {
    assert(getWindowRect(controlById(hwnd, id)).bottom <= text.top, "reading controls overlap translation text");
  }
  const work = getWorkArea();
  const rect = getWindowRect(hwnd);
  assert(
    rect.left >= work.left && rect.top >= work.top && rect.right <= work.right && rect.bottom <= work.bottom,
    "popup extends outside the work area",
  );
}

function chat(text = TEXT): Response {
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

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const pdf = tmpPath("translate-style.pdf");
  writeFileSync(pdf, makeTextPdf("The quick brown fox reads a book."));
  const routes = new Map<string, { received: MockRequest[]; reply: Reply }>();
  const results: { name: string; status: string; elapsedMs: number; reason?: string }[] = [];
  let ordinal = 0;
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    async fetch(request) {
      const route = routes.get(new URL(request.url).pathname);
      if (!route) return new Response("unknown style route", { status: 404 });
      const body = await request.json();
      route.received.push({ url: request.url, model: body.model, stream: body.stream });
      return route.reply();
    },
  });

  async function run(
    name: string,
    check: (context: Context) => Promise<void>,
    options: { configured?: boolean; settings?: string[]; reply?: Reply; saveSettings?: boolean } = {},
  ): Promise<void> {
    if (process.env.SUMATRA_STYLE_CASE && process.env.SUMATRA_STYLE_CASE !== name) return;
    const started = Date.now();
    const path = `/${++ordinal}-${name}`;
    const endpoint = `${server.url.origin}${path}`;
    const received: MockRequest[] = [];
    routes.set(path, { received, reply: options.reply ?? (() => chat()) });
    const configured = options.configured !== false;
    const appdata = writeAppdata(
      `translate-style-${ordinal}`,
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
      const frameBand = frameThemeBand(session, tmpPath(`translate-style-${name}-reader-before.png`));
      await drag(session);
      await check({ session, received, endpoint, appdata, frameBand });
      await quit(session);
      results.push({ name, status: "PASS", elapsedMs: Date.now() - started });
      console.log(`PASS: translate-style ${name}`);
    } catch (error) {
      results.push({ name, status: "FAIL", elapsedMs: Date.now() - started, reason: String(error) });
      console.error(`FAIL: translate-style ${name}: ${String(error)}`);
      for (const cls of ["SumatraTranslatePopup", "SumatraTranslateConfig"]) {
        const hwnd = findTopWindow(session.proc.pid!, cls);
        if (hwnd) capture(hwnd, tmpPath(`translate-style-${name}-${cls}.png`));
      }
      session.client.close();
      await killAndWait(session.proc);
    } finally {
      routes.delete(path);
      writeFileSync(tmpPath("translate-style-results.json"), JSON.stringify(results, null, 2));
    }
  }

  try {
    for (const theme of ["Light", "Dark"]) {
      await run(
        `${theme.toLowerCase()}-card`,
        async ({ session, received, frameBand }) => {
          const current = await expectText(session);
          await checkCard(session, theme.toLowerCase());
          const hwnd = popup(session);
          assert(
            !current.bold && current.fontWeight >= 300 && current.fontWeight < 600 && current.fontSize === 11,
            "default translation font is not regular 11pt",
          );
          assert(sendMessage(controlById(hwnd, ReadingControl.bold), BM_GETCHECK, 0, 0) === 0n, "B defaults on");
          checkChrome(session);
          const afterBand = frameThemeBand(session, tmpPath(`translate-style-${theme.toLowerCase()}-reader-after.png`));
          assert(Math.abs(afterBand - frameBand) <= 10, "showing the popup changed the reader chrome theme");
          assert(received.length === 1, "style rendering sent another translation request");
          writeFileSync(
            tmpPath(`translate-style-${theme.toLowerCase()}-theme.json`),
            JSON.stringify(
              { theme, readerBefore: frameBand, readerAfter: afterBand, readerThemeStable: true },
              null,
              2,
            ),
          );
        },
        { settings: [`Theme = ${theme}`] },
      );
    }

    const finish = gate();
    try {
      await run(
        "bold-during-stream",
        async ({ session, received }) => {
          const before = await waitState(
            session,
            (value) => value.visible && value.busy && value.text === LONG,
            "first streamed text was not visible while connection stayed open",
          );
          assert(!before.bold && before.fontWeight < 600, "stream did not start with regular weight");
          await nativeResize(session, 440, 310);
          const hwnd = popup(session);
          const edit = body(session);
          sendMessage(controlById(hwnd, 3003), BM_CLICK, 0, 0);
          assert((await state(session.client)).pinned, "pin control did not apply");
          sendMessage(edit, EM_SETSEL, 1, 5);
          sendMessage(edit, EM_LINESCROLL, 0, 3);
          const selection = sendMessage(edit, EM_GETSEL, 0, 0);
          const firstLine = Number(sendMessage(edit, EM_GETFIRSTVISIBLELINE, 0, 0));
          assert(firstLine > 0, "stream test did not establish a scrolled reading position");
          const size = dimensions(hwnd);
          for (const enabled of [true, false]) {
            sendMessage(controlById(hwnd, ReadingControl.bold), BM_CLICK, 0, 0);
            const changed = await waitState(
              session,
              (value) => value.bold === enabled && (enabled ? value.fontWeight >= 700 : value.fontWeight < 600),
              "B did not change the actual font weight",
            );
            assert(
              changed.busy &&
                changed.pinned &&
                changed.text === LONG &&
                changed.request === before.request &&
                changed.fontSize === before.fontSize &&
                !changed.autoSize &&
                received.length === 1,
              "bold toggle changed translation content, request, font size or reading state",
            );
            assert(sameSize(dimensions(hwnd), size), "bold toggle overwrote the manual size");
            assert(sendMessage(edit, EM_GETSEL, 0, 0) === selection, "bold toggle changed the text selection");
            assert(
              Number(sendMessage(edit, EM_GETFIRSTVISIBLELINE, 0, 0)) === firstLine,
              "bold toggle changed the scrolled reading position",
            );
            assert(
              (sendMessage(controlById(hwnd, ReadingControl.bold), BM_GETCHECK, 0, 0) === 1n) === enabled,
              "B checked state does not match actual font weight",
            );
            capture(hwnd, tmpPath(`translate-style-stream-${enabled ? "bold" : "regular"}.png`));
          }
          finish.release();
          const completed = await expectText(session, LONG + "完成。");
          assert(
            !completed.bold &&
              sameSize(dimensions(hwnd), size) &&
              completed.request === before.request &&
              received.length === 1,
            "stream completion lost the selected weight or manual size",
          );
          writeFileSync(
            tmpPath("translate-style-stream.json"),
            JSON.stringify(
              {
                actualWeightToggled: true,
                textUnchanged: true,
                oneRequest: true,
                busyAndPinPreserved: true,
                selectionPreserved: true,
                scrollPreserved: true,
                manualSizePreserved: true,
              },
              null,
              2,
            ),
          );
        },
        {
          reply: () => {
            let canceled = false;
            return new Response(
              new ReadableStream<Uint8Array>({
                async start(controller) {
                  try {
                    controller.enqueue(Buffer.from(delta(LONG)));
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
            );
          },
        },
      );
    } finally {
      finish.release();
    }

    await run(
      "bold-settings-and-persistence",
      async ({ session, received, endpoint, appdata }) => {
        const setup = await state(session.client);
        assert(!setup.bold && received.length === 0, "blank profile started bold or sent an API request");
        let form = await openConfig(session);
        assert(sendMessage(form.field("bold"), BM_GETCHECK, 0, 0) === 0n, "settings default bold is enabled");
        sendMessage(form.field("bold"), WM_KEYDOWN, VK_RETURN, 0);
        assert(
          isWindowVisible(form.hwnd) && sendMessage(form.field("bold"), BM_GETCHECK, 0, 0) === 1n,
          "Enter on the bold checkbox saved the form instead of toggling it",
        );
        await closeConfig(form, false);
        assert(!(await state(session.client)).bold && received.length === 0, "Cancel applied the bold draft");
        form = await openConfig(session);
        checkBox(form.field("bold"), true);
        await closeConfig(form, true);
        const blankSaved = await waitState(
          session,
          (value) => value.bold && value.fontWeight >= 700,
          "bold-only settings could not apply before provider setup",
        );
        assert(
          blankSaved.text === setup.text && blankSaved.request === setup.request && received.length === 0,
          "blank-provider bold save changed setup text or sent an API request",
        );

        form = await openConfig(session);
        sendText(form.field("url"), endpoint);
        sendText(form.field("model"), MODEL);
        sendText(form.field("key"), KEY);
        await closeConfig(form, true);
        const configured = await expectText(session);
        assert(configured.bold && received.length === 1, "provider setup did not preserve bold");
        for (const enabled of [false, true]) {
          form = await openConfig(session);
          checkBox(form.field("bold"), enabled);
          await closeConfig(form, true);
          const changed = await waitState(
            session,
            (value) => value.bold === enabled && (enabled ? value.fontWeight >= 700 : value.fontWeight < 600),
            "display-only bold save did not apply",
          );
          assert(
            changed.request === configured.request && changed.text === TEXT && received.length === 1,
            "display-only bold save retried translation",
          );
        }
        const settingsPath = join(appdata, "SumatraPDF-settings.txt");
        await pollUntil(
          () => (existsSync(settingsPath) ? readFileSync(settingsPath, "utf8") : ""),
          (text) => /TranslatePopupBold\s*=\s*true\b/.test(text),
          { timeoutMs: TIMEOUT, error: "bold preference was not persisted" },
        );
        await quit(session);
        const count = received.length;
        const restarted = await launchControlled(["-appdata", appdata, pdf]);
        try {
          await restarted.client.waitForRenderIdle();
          await restarted.client.setNotificationsEnabled(false);
          await drag(restarted);
          const loaded = await expectText(restarted);
          assert(loaded.bold && loaded.fontWeight >= 700 && received.length === count + 1, "bold did not reload");
          assert(
            sendMessage(controlById(popup(restarted), ReadingControl.bold), BM_GETCHECK, 0, 0) === 1n,
            "reloaded header bold state is unchecked",
          );
          const reloadedForm = await openConfig(restarted);
          assert(
            sendMessage(reloadedForm.field("bold"), BM_GETCHECK, 0, 0) === 1n,
            "reloaded settings bold is unchecked",
          );
          await closeConfig(reloadedForm, false);
          capture(popup(restarted), tmpPath("translate-style-bold-reloaded.png"));
          writeFileSync(
            tmpPath("translate-style-persistence.json"),
            JSON.stringify(
              {
                cancelDiscarded: true,
                blankProviderSave: true,
                configuredSaveNoRetry: true,
                persistedBold: true,
                reloadedActualWeight: loaded.fontWeight,
                reloadedHeaderCheck: true,
                reloadedSettingCheck: true,
                oneReloadRequest: true,
              },
              null,
              2,
            ),
          );
        } finally {
          await quit(restarted);
        }
      },
      { configured: false, saveSettings: true },
    );

    await run(
      "bold-minimum-layout",
      async ({ session, received }) => {
        const current = await expectText(session, LONG);
        assert(current.bold && current.fontWeight >= 700 && current.fontSize === 32, "large bold font was not loaded");
        checkChrome(session);
        assert(
          getClientRect(body(session)).bottom >= current.fontPixels,
          "minimum window cannot show one full text line",
        );
        assert(Number(sendMessage(body(session), EM_GETLINECOUNT, 0, 0)) > 1, "large text did not wrap");
        sendMessage(body(session), EM_LINESCROLL, 0, 2);
        assert(Number(sendMessage(body(session), EM_GETFIRSTVISIBLELINE, 0, 0)) > 0, "large bold text cannot scroll");
        const hwnd = popup(session);
        const size = dimensions(hwnd);
        sendMessage(controlById(hwnd, ReadingControl.bold), BM_CLICK, 0, 0);
        const regular = await waitState(
          session,
          (value) => !value.bold && value.fontWeight < 600,
          "minimum-size B toggle failed",
        );
        assert(
          regular.fontSize === 32 &&
            regular.request === current.request &&
            received.length === 1 &&
            sameSize(dimensions(hwnd), size),
          "minimum-size B toggle changed manual geometry or sent request",
        );
        checkChrome(session);
        capture(hwnd, tmpPath("translate-style-minimum-layout.png"));
      },
      {
        settings: [
          "TranslatePopupBold = true",
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
  assert(results.length > 0, "no style scenario selected");
  assert(
    results.every((item) => item.status === "PASS"),
    `${results.filter((item) => item.status === "FAIL").length} style scenarios failed`,
  );
}

if (import.meta.main) await runStandalone(testit);
