// SPDX-License-Identifier: GPL-3.0-only
// Run: bun tests/ad-hoc-llm-translate.ts --no-build
import { writeFileSync } from "node:fs";
import { ControlClient, ControlCommand } from "./control.ts";
import { assemblePdf, pollUntil, runStandalone, tmpPath, writeAppdata } from "./util.ts";
import { findCanvas, killAndWait, launchControlled, pressEscape } from "./win-automation.ts";
import { controlById, defaultTranslationPrompt, TranslateControl } from "./translate-test-controls.ts";
import {
  captureWindowToPng,
  enumChildWindows,
  enumWindows,
  findTopWindow,
  getClassName,
  getClientRect,
  getClipboardText,
  getWindowLong,
  getWindowOwner,
  getWindowPid,
  getWindowRect,
  getWindowText,
  getWorkArea,
  GWL_STYLE,
  hasInteractiveDesktop,
  isWindowVisible,
  MK_LBUTTON,
  packCoords,
  sendMessage,
  sendText,
  setProcessDpiAware,
  setWindowPos,
  SWP_NOACTIVATE,
  SWP_NOZORDER,
  WM_LBUTTONDOWN,
  WM_LBUTTONUP,
} from "./winapi.ts";

const MODEL = 'mock-"model"\\path';
const TEST_KEY = "translation-test-not-a-secret";
const LINE = 'The quick brown fox reads "quoted" path\\name.';
const TIMEOUT = 8000;
const SWP_NOSIZE = 0x0001;
const SWP_NOREDRAW = 0x0008;
const ES_PASSWORD = 0x0020;
const EM_GETPASSWORDCHAR = 0x00d2;
const BM_CLICK = 0x00f5;

type PopupState = {
  raw: string;
  visible: boolean;
  busy: boolean;
  pinned: boolean;
  error: boolean;
  request: number;
  text: string;
  status: string;
  rect: number[];
};
type Session = Awaited<ReturnType<typeof launchControlled>>;
type MockRequest = { body: any; authorization: string | null; url: string };
type Reply = (request: MockRequest) => Response;

function assert(ok: unknown, message: string): asserts ok {
  if (!ok) throw new Error(`llm-translate: ${message}`);
}

function parseState(raw: string): PopupState {
  const fields = Object.fromEntries(
    raw.split(/\r?\n/).flatMap((line) => {
      const eq = line.indexOf("=");
      return eq < 0 ? [] : [[line.slice(0, eq), line.slice(eq + 1)]];
    }),
  );
  const decoded = (key: string) => (fields[key] ? String(JSON.parse(fields[key])) : "");
  return {
    raw,
    visible: fields.visible === "1",
    busy: fields.busy === "1",
    pinned: fields.pinned === "1",
    error: fields.error === "1",
    request: Number(fields.request ?? 0),
    text: decoded("text"),
    status: decoded("status"),
    rect: fields.rect?.split(/[ ,]+/).map(Number) ?? [],
  };
}

async function state(client: ControlClient, action = ""): Promise<PopupState> {
  const result = await client.request(ControlCommand.TestTranslatePopup, action ? [action] : []);
  assert(result[0] === 0, `popup request failed: ${String(result[1] ?? "")}`);
  return parseState(String(result[1] ?? ""));
}

async function waitState(
  client: ControlClient,
  predicate: (value: PopupState) => boolean,
  description: string,
): Promise<PopupState> {
  return pollUntil(() => state(client), predicate, {
    timeoutMs: TIMEOUT,
    intervalMs: 20,
    error: (last) => `${description}\n${last?.raw ?? "no state"}`,
  });
}

async function drag(client: ControlClient, word: string): Promise<void> {
  const result = await client.request(ControlCommand.TestRotatedTextMouseDrag, [word]);
  const raw = String(result[1] ?? "");
  assert(raw.includes(`selected=${word}\n`), `mouse release did not select ${JSON.stringify(word)}:\n${raw}`);
}

async function checkPopupMove(session: Session, received: MockRequest[]): Promise<void> {
  await session.client.waitForRenderIdle();
  const before = await state(session.client);
  const frame = getWindowRect(session.frame);
  const work = getWorkArea();
  const [x, y, width, height] = before.rect as [number, number, number, number];
  let dx = 0;
  let dy = work.bottom - y - height >= 29 ? 29 : y - work.top >= 29 ? -29 : 0;
  if (dy === 0) dx = work.right - x - width >= 31 ? 31 : x - work.left >= 31 ? -31 : 0;
  assert(dx !== 0 || dy !== 0, "no room to move popup within the primary monitor work area");
  const flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOACTIVATE;
  const count = received.length;
  try {
    assert(setWindowPos(session.frame, frame.left + dx, frame.top + dy, 0, 0, flags), "frame move failed");
    // Read before any render-idle wait: WM_MOVE must update the popup without a canvas repaint.
    const moved = await state(session.client);
    const actual = getWindowRect(session.frame);
    const frameDx = actual.left - frame.left;
    const frameDy = actual.top - frame.top;
    assert(frameDx !== 0 || frameDy !== 0, "frame did not move");
    assert(
      moved.visible && moved.rect[0] === x + frameDx && moved.rect[1] === y + frameDy,
      `popup did not follow the frame without repaint:\n${before.raw}\n${moved.raw}`,
    );
    assert(
      moved.rect[0]! >= work.left &&
        moved.rect[1]! >= work.top &&
        moved.rect[0]! + moved.rect[2]! <= work.right &&
        moved.rect[1]! + moved.rect[3]! <= work.bottom,
      `moved popup escaped the primary monitor work area:\n${moved.raw}`,
    );
    await session.client.waitForRenderIdle();
    const settled = await state(session.client);
    assert(
      settled.request === before.request && received.length === count && !settled.busy && settled.text === before.text,
      "moving the frame started another translation",
    );
  } finally {
    assert(setWindowPos(session.frame, frame.left, frame.top, 0, 0, flags), "could not restore frame position");
  }
  const restored = await state(session.client);
  assert(
    restored.rect[0] === x &&
      restored.rect[1] === y &&
      restored.request === before.request &&
      received.length === count,
    "restoring the frame changed translation or left the popup behind",
  );
}

async function openPopupConfig(session: Session): Promise<number> {
  await state(session.client, "settings");
  return pollUntil(
    () => findTopWindow(session.proc.pid!, "SumatraTranslateConfig"),
    (hwnd) => hwnd !== 0 && isWindowVisible(hwnd),
    { timeoutMs: TIMEOUT, error: "native translation settings window did not open" },
  );
}

async function savePopupConfig(config: number, url: string, model: string, key: string): Promise<void> {
  const edits = [TranslateControl.url, TranslateControl.model, TranslateControl.key].map((id) =>
    controlById(config, id),
  );
  const save = controlById(config, TranslateControl.save);
  assert((getWindowLong(edits[2]!, GWL_STYLE) & ES_PASSWORD) !== 0, "API key field has no password style");
  assert(sendMessage(edits[2]!, EM_GETPASSWORDCHAR, 0, 0) !== 0n, "API key field has no masking character");
  sendText(edits[0]!, url);
  sendText(edits[1]!, model);
  sendText(edits[2]!, key);
  sendMessage(save, BM_CLICK, 0, 0);
  await pollUntil(
    () => isWindowVisible(config),
    (visible) => !visible,
    {
      timeoutMs: TIMEOUT,
      error: "native settings Save did not close the form",
    },
  );
}

function makePdf(): string {
  const escaped = LINE.replace(/[\\()]/g, "\\$&");
  const content = `BT /F1 18 Tf 72 700 Td (${escaped}) Tj ET`;
  return assemblePdf([
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    `<< /Length ${content.length} >>\nstream\n${content}\nendstream`,
  ]);
}

function event(content: string, finish: string | null = null): string {
  return `data: ${JSON.stringify({ choices: [{ delta: { content }, finish_reason: finish }] })}\r\n\r\n`;
}

function stream(parts: { bytes: string; delay?: number; wait?: Promise<void> }[], split = false): Response {
  let canceled = false;
  const body = new ReadableStream<Uint8Array>({
    async start(controller) {
      try {
        for (const part of parts) {
          if (part.wait) await part.wait;
          if (part.delay) await Bun.sleep(part.delay);
          if (canceled) return;
          const bytes = Buffer.from(part.bytes);
          if (split) {
            // Each Chinese code point and each CRLF pair crosses writes.
            for (let offset = 0; offset < bytes.length; offset++) {
              if (canceled) return;
              controller.enqueue(bytes.subarray(offset, offset + 1));
              if (offset % 11 === 0) await Bun.sleep(1);
            }
          } else {
            controller.enqueue(bytes);
          }
        }
        controller.close();
      } catch (error) {
        if (!canceled) controller.error(error);
      }
    },
    cancel() {
      canceled = true;
    },
  });
  return new Response(body, { headers: { "Content-Type": "text/event-stream; charset=utf-8" } });
}

function jsonReply(text: string): Response {
  return Response.json({ choices: [{ message: { content: text }, finish_reason: "stop" }] });
}

function rawJson(contentJson: string): Response {
  return new Response(`{"choices":[{"message":{"content":${contentJson}},"finish_reason":"stop"}]}`, {
    headers: { "Content-Type": "application/json" },
  });
}

function rawSse(contentJson: string): Response {
  return stream([
    { bytes: `data: {"choices":[{"delta":{"content":${contentJson}},"finish_reason":null}]}\r\n\r\n` },
    { bytes: event("", "stop") + "data: [DONE]\r\n\r\n" },
  ]);
}

async function quit(session: Session): Promise<void> {
  await session.client.quit();
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    const exit = await Promise.race([
      session.proc.exited,
      new Promise<never>((_, reject) => {
        timer = setTimeout(() => reject(new Error("translation worker blocked process exit")), TIMEOUT);
      }),
    ]);
    assert(exit === 0, `reader exited with code ${exit}`);
  } finally {
    clearTimeout(timer);
  }
}

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const pdf = tmpPath("llm-translate.pdf");
  const otherPdf = tmpPath("llm-translate-other.pdf");
  writeFileSync(pdf, makePdf(), "latin1");
  writeFileSync(otherPdf, makePdf(), "latin1");
  const routes = new Map<string, Reply>();
  const requests = new Map<string, MockRequest[]>();
  const results: { name: string; status: string; elapsedMs: number; reason?: string }[] = [];
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    async fetch(request) {
      const path = new URL(request.url).pathname;
      const route = routes.get(path);
      if (!route) return new Response("unknown test route", { status: 404 });
      try {
        const item = {
          body: await request.json(),
          authorization: request.headers.get("authorization"),
          url: request.url,
        };
        requests.get(path)!.push(item);
        return route(item);
      } catch {
        return Response.json({ error: { message: "Invalid JSON request" } }, { status: 400 });
      }
    },
  });
  let ordinal = 0;

  async function run(
    name: string,
    reply: Reply,
    check: (session: Session, received: MockRequest[], endpoint: string) => Promise<void>,
    options: { configured?: boolean; toolbar?: boolean; twoTabs?: boolean; enabled?: boolean } = {},
  ): Promise<void> {
    const started = Date.now();
    const path = `/${++ordinal}-${name}`;
    const received: MockRequest[] = [];
    routes.set(path, reply);
    requests.set(path, received);
    const dir = writeAppdata(
      `llm-translate-appdata-${ordinal}`,
      [
        "RestoreSession = false",
        "CheckForUpdates = false",
        "ShowToc = false",
        "WindowState = 1",
        "WindowPos = 50 50 1000 750",
        `SelectionToolbar = ${options.toolbar !== false}`,
        `TranslatePopupEnabled = ${options.enabled !== false}`,
        `TranslatePopupUrl = ${options.configured === false ? "" : `${server.url.origin}${path}`}`,
        `TranslatePopupModel = ${MODEL}`,
        `TranslatePopupApiKey = ${TEST_KEY}`,
      ].join("\n"),
    );
    const session = await launchControlled(["-appdata", dir, pdf, ...(options.twoTabs ? [otherPdf] : [])], {
      defaultWindowPos: true,
    });
    try {
      await session.client.waitForRenderIdle();
      await session.client.setNotificationsEnabled(false);
      await check(session, received, `${server.url.origin}${path}`);
      await quit(session);
      results.push({ name, status: "PASS", elapsedMs: Date.now() - started });
      console.log(`PASS: llm-translate ${name}`);
    } catch (error) {
      results.push({ name, status: "FAIL", elapsedMs: Date.now() - started, reason: String(error) });
      session.client.close();
      await killAndWait(session.proc);
      throw error;
    } finally {
      routes.delete(path);
      writeFileSync(tmpPath("llm-translate-results.json"), JSON.stringify(results, null, 2));
    }
  }

  async function expectText(session: Session, expected: string): Promise<PopupState> {
    const value = await waitState(session.client, (s) => s.visible && !s.busy && s.text === expected, expected);
    assert(!value.error, `successful response marked as error:\n${value.raw}`);
    return value;
  }

  async function expectError(session: Session): Promise<PopupState> {
    const value = await waitState(session.client, (s) => s.visible && !s.busy && s.error, "expected visible API error");
    assert(!value.raw.includes(TEST_KEY), "API key leaked into popup diagnostics");
    return value;
  }

  try {
    let releaseStream!: () => void;
    let streamReleased = false;
    const streamGate = new Promise<void>((resolve) => {
      releaseStream = () => {
        streamReleased = true;
        resolve();
      };
    });
    try {
      await run(
        "stream-and-actions",
        () => stream([{ bytes: event("译") }, { bytes: event("文：中文流式翻译。", "stop"), wait: streamGate }], true),
        async (session, received) => {
          await drag(session.client, "quick");
          await waitState(
            session.client,
            (s) => s.visible && s.busy && s.text === "译",
            "first streamed Chinese character",
          );
          assert(!streamReleased, "server finished the stream before the intermediate text was observed");
          releaseStream();
          const finished = await expectText(session, "译文：中文流式翻译。");
          assert(received.length === 1, `expected one request, got ${received.length}`);
          const first = received[0]!;
          assert(first.body.model === MODEL && first.body.stream === true, "model escaping or streaming flag changed");
          assert(
            first.body.messages[1]?.content === defaultTranslationPrompt("quick"),
            "default prompt or selected PDF text changed",
          );
          assert(
            first.body.messages[0]?.content.includes("Simplified Chinese"),
            "missing Chinese translation instruction",
          );
          assert(first.authorization === `Bearer ${TEST_KEY}`, "authorization header changed");
          assert(
            finished.rect.length === 4 && finished.rect[2]! > 0 && finished.rect[3]! > 0,
            "popup has no screen rectangle",
          );
          await checkPopupMove(session, received);
          await state(session.client, "copy");
          if (hasInteractiveDesktop()) {
            assert((await getClipboardText()) === finished.text, "copy did not put translation on clipboard");
          } else {
            const reason = "Session is locked or disconnected; clipboard content readback was not tested.";
            results.push({ name: "clipboard-content-readback", status: "SKIP", elapsedMs: 0, reason });
            console.log(`SKIP: llm-translate clipboard-content-readback: ${reason}`);
          }
          captureWindowToPng(session.frame, tmpPath("llm-translate-reader.png"));
          enumWindows((hwnd) => {
            if (
              getWindowPid(hwnd) === session.proc.pid &&
              getWindowOwner(hwnd) === session.frame &&
              isWindowVisible(hwnd) &&
              /Translate/i.test(getClassName(hwnd))
            ) {
              captureWindowToPng(hwnd, tmpPath("llm-translate-popup.png"));
            }
            return true;
          });
          assert((await state(session.client, "pin")).pinned, "pin did not enable fixed state");
          await pressEscape(session.frame);
          await waitState(session.client, (s) => !s.visible, "Esc should dismiss a pinned popup");
        },
        { toolbar: false },
      );
    } finally {
      releaseStream();
    }

    await run(
      "request-escaping",
      () => jsonReply("引号与路径。"),
      async (session, received) => {
        const source = '"quoted" path\\name.';
        await drag(session.client, source);
        await expectText(session, "引号与路径。");
        assert(
          received[0]?.body.messages[1]?.content === defaultTranslationPrompt(source),
          "quotes or backslashes changed during JSON encoding",
        );
      },
    );

    const initialJsonText = "非流式中文响应。";
    const retryJsonText = "重试后的非流式中文响应。";
    let jsonReplies = 0;
    await run(
      "json-fallback",
      () => jsonReply(++jsonReplies === 1 ? initialJsonText : retryJsonText),
      async (session, received, endpoint) => {
        await drag(session.client, "quick");
        const first = await expectText(session, initialJsonText);
        const firstCount = received.length;
        assert(firstCount === 1, `expected one initial JSON request, got ${firstCount}`);
        const firstRequest = received[0]!;
        assert(
          firstRequest.body.model === MODEL &&
            firstRequest.body.stream === true &&
            firstRequest.body.messages[1]?.content === defaultTranslationPrompt("quick"),
          "initial JSON request parameters or selected text changed",
        );
        await state(session.client, "retry");
        const retried = await waitState(
          session.client,
          (s) =>
            s.visible &&
            !s.busy &&
            s.request > first.request &&
            received.length === firstCount + 1 &&
            s.text === retryJsonText,
          "retry should complete a new request with a different translation",
        );
        assert(!retried.error, `retried response marked as error:\n${retried.raw}`);
        const retryRequest = received[firstCount]!;
        assert(
          JSON.stringify(retryRequest.body) === JSON.stringify(firstRequest.body),
          "retry changed the model, prompt or selected text",
        );
        assert(
          retryRequest.url === endpoint && retryRequest.authorization === `Bearer ${TEST_KEY}`,
          "retry changed the endpoint or authorization",
        );
        await state(session.client, "close");
        await waitState(session.client, (s) => !s.visible, "close should hide popup");
      },
    );

    const escapedMath = String.raw`"变量 \ud835\udc65"`;
    const literalEscape = String.raw`"literal \\ud835\\udc65"`;
    for (const [kind, response] of [
      ["json", rawJson],
      ["sse", rawSse],
    ] as const) {
      await run(
        `${kind}-surrogate-pair`,
        () => response(escapedMath),
        async (session) => {
          await drag(session.client, "quick");
          await expectText(session, "变量 𝑥");
        },
      );
      await run(
        `${kind}-literal-unicode`,
        () => response(literalEscape),
        async (session) => {
          await drag(session.client, "quick");
          await expectText(session, String.raw`literal \ud835\udc65`);
        },
      );
      for (const [label, malformed] of [
        ["high", String.raw`"\ud835"`],
        ["low", String.raw`"\udc65"`],
      ] as const) {
        await run(
          `${kind}-unpaired-${label}`,
          () => response(malformed),
          async (session) => {
            await drag(session.client, "quick");
            await expectError(session);
          },
        );
      }
    }

    await run(
      "http-error-redaction",
      () => Response.json({ error: { message: `Rejected credential ${TEST_KEY}` } }, { status: 401 }),
      async (session) => {
        await drag(session.client, "quick");
        const value = await expectError(session);
        assert(`${value.text} ${value.status}`.includes("401"), "HTTP status missing from error");
      },
    );

    await run(
      "api-error",
      () =>
        stream([
          {
            bytes: `event: error\r\ndata: ${JSON.stringify({ error: { message: `API rejected ${TEST_KEY}` } })}\r\n\r\n`,
          },
        ]),
      async (session) => {
        await drag(session.client, "quick");
        await expectError(session);
      },
    );

    for (const [name, reply] of [
      ["output-limit", () => stream([{ bytes: event("不完整译文", "length") }])],
      ["empty-response", () => jsonReply("")],
      ["interrupted-stream", () => stream([{ bytes: event("未完成") }])],
    ] as const) {
      await run(name, reply, async (session) => {
        await drag(session.client, "quick");
        await expectError(session);
      });
    }

    await run(
      "replace-selection",
      (request) =>
        request.body.messages[1]?.content === defaultTranslationPrompt("quick")
          ? stream([{ bytes: event("旧") }, { bytes: event("结果", "stop"), delay: 1000 }])
          : jsonReply("新选区译文。"),
      async (session, received) => {
        await drag(session.client, "quick");
        const old = await waitState(session.client, (s) => s.busy && s.text === "旧", "old request should start");
        await drag(session.client, "brown");
        const current = await expectText(session, "新选区译文。");
        assert(current.request !== old.request, "new selection reused the old request identity");
        await Bun.sleep(1200);
        const afterOld = await state(session.client);
        assert(afterOld.text === current.text && !afterOld.busy, "late result overwrote the new selection");
        assert(received.length === 2, `expected two requests, got ${received.length}`);
      },
    );

    await run(
      "switch-tab",
      () => stream([{ bytes: event("翻译") }, { bytes: event("完成", "stop"), delay: 800 }]),
      async (session) => {
        await drag(session.client, "quick");
        await waitState(session.client, (s) => s.busy, "tab test request should start");
        await state(session.client, "pin");
        const current = await session.client.request(ControlCommand.TestSelectTab, [-1]);
        const match = /(?:^|\s)(?:idx|current)=(\d+)/.exec(String(current[1]));
        const selected = match ? Number(match[1]) : 1;
        await session.client.request(ControlCommand.TestSelectTab, [selected === 0 ? 1 : 0]);
        await waitState(session.client, (s) => !s.visible && !s.busy, "tab switch must hide even a pinned translation");
      },
      { twoTabs: true },
    );

    await run(
      "missing-configuration-and-settings-ui",
      () => jsonReply("设置保存后的译文。"),
      async (session, received, endpoint) => {
        await drag(session.client, "quick");
        const value = await waitState(session.client, (s) => s.visible && !s.busy, "configuration prompt");
        assert(/设置|配置/.test(`${value.text} ${value.status}`), "missing configuration has no setup instruction");
        assert(received.length === 0, "unconfigured popup sent a model request");
        const newModel = 'ui-"model"\\configured';
        const newKey = "translation-settings-ui-not-a-secret";
        const newUrl = `${endpoint}?configured=ui`;
        await savePopupConfig(await openPopupConfig(session), newUrl, newModel, newKey);
        await expectText(session, "设置保存后的译文。");
        assert(received.length === 1, "saving settings did not start exactly one translation");
        assert(received[0]!.url === newUrl, "saved endpoint did not reach the network layer");
        assert(received[0]!.body.model === newModel, "saved model did not reach the network layer");
        assert(received[0]!.authorization === `Bearer ${newKey}`, "saved API key did not reach the network layer");
      },
      { configured: false },
    );

    for (const dismiss of ["clear", "escape", "pinned-clear"] as const) {
      await run(
        `settings-save-after-${dismiss}`,
        () => stream([{ bytes: event("进行中") }, { bytes: event("结束", "stop"), delay: TIMEOUT }]),
        async (session, received, endpoint) => {
          await drag(session.client, "quick");
          const initial = await waitState(
            session.client,
            (s) => s.busy && s.text === "进行中",
            "request did not start",
          );
          const pinned = dismiss === "pinned-clear";
          if (pinned) await state(session.client, "pin");
          const config = await openPopupConfig(session);

          if (dismiss === "escape") {
            await pressEscape(session.frame);
          } else {
            const canvas = findCanvas(session.frame);
            const rect = getClientRect(canvas);
            const point = packCoords(Math.round(rect.right / 2), Math.round(rect.bottom / 2));
            sendMessage(canvas, WM_LBUTTONDOWN, MK_LBUTTON, point);
            sendMessage(canvas, WM_LBUTTONUP, 0, point);
          }
          await session.client.waitForRenderIdle();
          assert(isWindowVisible(config), "dismissing the selection closed the settings window");
          const before = await state(session.client);
          await savePopupConfig(config, `${endpoint}?updated=1`, MODEL, TEST_KEY);
          const after = await state(session.client);

          assert(
            after.request === initial.request && received.length === 1,
            "saving settings resubmitted the old selection",
          );
          assert(
            before.visible === pinned && after.visible === pinned,
            "settings changed the popup dismissal behavior",
          );
          assert(pinned || (!before.busy && !after.busy), "dismissing the popup did not cancel the request");
        },
      );
    }

    await run(
      "disabled",
      () => jsonReply("不得请求此接口"),
      async (session, received) => {
        await drag(session.client, "quick");
        const value = await state(session.client);
        assert(
          !value.visible && !value.busy && received.length === 0,
          "disabled translation still showed or requested",
        );
      },
      { enabled: false },
    );

    await run(
      "quit-during-stream",
      () => stream([{ bytes: event("尚未完成") }, { bytes: event("最终文本", "stop"), delay: 5000 }]),
      async (session) => {
        await drag(session.client, "quick");
        await waitState(session.client, (s) => s.busy && s.text !== "", "request should be active before quit");
      },
    );
  } finally {
    server.stop(true);
    writeFileSync(tmpPath("llm-translate-results.json"), JSON.stringify(results, null, 2));
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
