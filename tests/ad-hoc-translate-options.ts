// SPDX-License-Identifier: GPL-3.0-only
// Run: bun tests/ad-hoc-translate-options.ts --no-build
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand } from "./control.ts";
import { makeTextPdf, pollUntil, runStandalone, tmpPath, writeAppdata } from "./util.ts";
import { killAndWait, launchControlled } from "./win-automation.ts";
import {
  captureWindowToPng,
  enumChildWindows,
  findTopWindow,
  getClassName,
  getControlText,
  getWindowLong,
  isWindowVisible,
  repaintWindow,
  sendMessage,
  sendText,
  setProcessDpiAware,
} from "./winapi.ts";
import {
  comboItems,
  controlById,
  ParamControl,
  ReadingControl,
  selectCombo,
  TranslateControl,
} from "./translate-test-controls.ts";

const TIMEOUT = 8000;
const MODEL = "options-test-model";
const KEY = "options-test-not-a-secret";
const TRANSLATION = "选项测试译文。";
const BM_CLICK = 0x00f5;
const BM_GETCHECK = 0x00f0;
const BM_SETCHECK = 0x00f1;
const EM_SETSEL = 0x00b1;
const WM_CHAR = 0x0102;
const WM_CLOSE = 0x0010;
const VARIABLE_PROMPT = "${langFrom}|${langTo}|${sourceText}";
const GWL_ID = -12;

type Session = Awaited<ReturnType<typeof launchControlled>>;
type Form = { hwnd: number; field: (name: keyof typeof TranslateControl) => number };
type PopupState = { visible: boolean; busy: boolean; error: boolean; text: string; status: string; request: number };
type MockRequest = { body: any; raw: string; url: string };
type Context = { session: Session; received: MockRequest[]; endpoint: string; appdata: string };
type Reply = (request: MockRequest) => Response;

function assert(ok: unknown, message: string): asserts ok {
  if (!ok) throw new Error(`translate-options: ${message}`);
}

async function state(client: ControlClient, action = ""): Promise<PopupState> {
  const result = await client.request(ControlCommand.TestTranslatePopup, action ? [action] : []);
  assert(result[0] === 0, "popup diagnostic failed");
  const fields = Object.fromEntries(
    String(result[1] ?? "")
      .split(/\r?\n/)
      .flatMap((line) => {
        const eq = line.indexOf("=");
        return eq < 0 ? [] : [[line.slice(0, eq), line.slice(eq + 1)]];
      }),
  );
  return {
    visible: fields.visible === "1",
    busy: fields.busy === "1",
    error: fields.error === "1",
    text: JSON.parse(fields.text || '""'),
    status: JSON.parse(fields.status || '""'),
    request: Number(fields.request || 0),
  };
}

async function drag(session: Session, text = "quick"): Promise<void> {
  const result = await session.client.request(ControlCommand.TestRotatedTextMouseDrag, [text]);
  assert(
    String(result[1] ?? "").includes(`selected=${text}\n`),
    "native mouse release did not select the requested text",
  );
}

async function openConfig(session: Session): Promise<Form> {
  await state(session.client, "settings");
  const hwnd = await pollUntil(
    () => findTopWindow(session.proc.pid!, "SumatraTranslateConfig"),
    (value) => value !== 0 && isWindowVisible(value),
    { timeoutMs: TIMEOUT, error: "native settings did not open" },
  );
  return { hwnd, field: (name) => controlById(hwnd, TranslateControl[name]) };
}

async function openParams(session: Session, form: Form): Promise<number> {
  sendMessage(form.field("custom"), BM_CLICK, 0, 0);
  return pollUntil(
    () => findTopWindow(session.proc.pid!, "SumatraTranslateParams"),
    (value) => value !== 0 && isWindowVisible(value),
    { timeoutMs: TIMEOUT, error: "native custom-parameters editor did not open" },
  );
}

async function waitClosed(hwnd: number): Promise<void> {
  await pollUntil(
    () => isWindowVisible(hwnd),
    (visible) => !visible,
    {
      timeoutMs: TIMEOUT,
      error: "native settings did not close",
    },
  );
}

function fillConnection(form: Form, endpoint: string): void {
  sendText(form.field("url"), endpoint);
  sendText(form.field("model"), MODEL);
  sendText(form.field("key"), KEY);
}

function fillParams(hwnd: number, rows: [string, string][]): void {
  rows.forEach(([name, value], index) => {
    if (index > 0) sendMessage(controlById(hwnd, ParamControl.add), BM_CLICK, 0, 0);
    sendText(controlById(hwnd, 3300 + 3 * index), name);
    sendText(controlById(hwnd, 3301 + 3 * index), value);
  });
}

function paramsValues(hwnd: number): [string, string][] {
  const rows: [string, string][] = [];
  for (let i = 0; i < 64; i++) {
    try {
      rows.push([getControlText(controlById(hwnd, 3300 + 3 * i)), getControlText(controlById(hwnd, 3301 + 3 * i))]);
    } catch {
      break;
    }
  }
  return rows;
}

async function expectText(session: Session, text = TRANSLATION): Promise<PopupState> {
  const value = await pollUntil(
    () => state(session.client),
    (current) => current.visible && !current.busy && current.text === text,
    { timeoutMs: TIMEOUT, error: "translation did not finish with the expected text" },
  );
  assert(!value.error, "valid translation was marked as an error");
  return value;
}

async function expectError(session: Session): Promise<PopupState> {
  const value = await pollUntil(
    () => state(session.client),
    (current) => current.visible && !current.busy && current.error,
    { timeoutMs: TIMEOUT, error: "incomplete or failed response was accepted" },
  );
  assert(value.text.length > 0 && !value.text.includes(KEY), "error is empty or contains an API key");
  return value;
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
        throw new Error("reader did not exit");
      }),
    ]);
    assert(code === 0, "reader exited unsuccessfully");
  } catch (error) {
    session.client.close();
    await killAndWait(session.proc);
    throw error;
  }
}

function chatReply(text = TRANSLATION): Response {
  return Response.json({ choices: [{ message: { content: text }, finish_reason: "stop" }] });
}

function responseOutput(text: string): object[] {
  return [{ type: "message", role: "assistant", content: [{ type: "output_text", text }] }];
}

function event(type: string, fields: Record<string, unknown>): string {
  return `event: ${type}\r\ndata: ${JSON.stringify({ type, ...fields })}\r\n\r\n`;
}

function stream(parts: { bytes: string; wait?: Promise<void> }[]): Response {
  let canceled = false;
  const body = new ReadableStream<Uint8Array>({
    async start(controller) {
      try {
        for (const part of parts) {
          if (part.wait) await part.wait;
          if (canceled) return;
          const bytes = Buffer.from(part.bytes);
          for (let offset = 0; offset < bytes.length; offset += 7) {
            if (canceled) return;
            controller.enqueue(bytes.subarray(offset, offset + 7));
            if (offset % 49 === 0) await Bun.sleep(1);
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

export async function testit(): Promise<void> {
  setProcessDpiAware();
  const pdf = tmpPath("translate-options.pdf");
  writeFileSync(pdf, makeTextPdf("The quick brown fox reads ${langTo} beside a book."));
  const routes = new Map<string, { received: MockRequest[]; reply: Reply }>();
  const results: { name: string; status: string; elapsedMs: number; reason?: string }[] = [];
  let ordinal = 0;
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    async fetch(request) {
      const route = routes.get(new URL(request.url).pathname);
      if (!route) return new Response("unknown native test route", { status: 404 });
      const raw = await request.text();
      const item = { body: JSON.parse(raw), raw, url: request.url };
      route.received.push(item);
      return route.reply(item);
    },
  });

  async function run(
    name: string,
    check: (context: Context) => Promise<void>,
    options: {
      configured?: boolean;
      responses?: boolean;
      reply?: Reply;
      settings?: string[];
      saveSettings?: boolean;
    } = {},
  ): Promise<void> {
    if (process.env.SUMATRA_OPTIONS_CASE && process.env.SUMATRA_OPTIONS_CASE !== name) return;
    const started = Date.now();
    const path = `/${++ordinal}-${name}${options.responses ? "/responses" : ""}`;
    const endpoint = `${server.url.origin}${path}?native-options=1`;
    const received: MockRequest[] = [];
    routes.set(path, { received, reply: options.reply ?? (() => chatReply()) });
    const appdata = writeAppdata(
      `translate-options-${ordinal}`,
      [
        "RestoreSession = false",
        "CheckForUpdates = false",
        "ShowToc = false",
        "SelectionToolbar = false",
        "TranslatePopupEnabled = true",
        `TranslatePopupUrl = ${options.configured ? endpoint : ""}`,
        `TranslatePopupModel = ${options.configured ? MODEL : ""}`,
        `TranslatePopupApiKey = ${options.configured ? KEY : ""}`,
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
      console.log(`PASS: translate-options ${name}`);
    } catch (error) {
      results.push({ name, status: "FAIL", elapsedMs: Date.now() - started, reason: String(error) });
      console.error(`FAIL: translate-options ${name}: ${String(error)}`);
      for (const cls of ["SumatraTranslateConfig", "SumatraTranslateParams", "SumatraTranslatePopup"]) {
        const hwnd = findTopWindow(session.proc.pid!, cls);
        if (hwnd) captureWindowToPng(hwnd, tmpPath(`translate-options-${name}-${cls}.png`));
      }
      session.client.close();
      await killAndWait(session.proc);
    } finally {
      routes.delete(path);
      writeFileSync(tmpPath("translate-options-results.json"), JSON.stringify(results, null, 2));
    }
  }

  try {
    await run("defaults", async ({ session, received }) => {
      const form = await openConfig(session);
      for (const name of Object.keys(TranslateControl) as (keyof typeof TranslateControl)[]) form.field(name);
      assert(
        getControlText(form.field("url")) === "" && getControlText(form.field("model")) === "",
        "provider defaults are not blank",
      );
      assert(getControlText(form.field("source")) === "Auto", "source language does not default to Auto");
      assert(
        getControlText(form.field("target")) === "中文（简体）",
        "target language does not default to Simplified Chinese",
      );
      assert(/^Off\b/.test(getControlText(form.field("thinking"))), "thinking does not default to Off");
      assert(Number(getControlText(form.field("temperature"))) === 1, "temperature does not default to 1");
      assert(sendMessage(form.field("stream"), BM_GETCHECK, 0, 0) === 1n, "streaming is not enabled by default");
      assert(!comboItems(form.field("target")).includes("Auto"), "target language incorrectly permits Auto");
      const prompt = getControlText(form.field("prompt"));
      assert(
        prompt.includes("${langFrom}") && prompt.includes("${langTo}") && prompt.includes("${sourceText}"),
        "default prompt variables are missing",
      );
      sendMessage(form.field("prompt"), EM_SETSEL, -1, -1);
      sendMessage(form.field("prompt"), WM_CHAR, 13, 0);
      assert(
        isWindowVisible(form.hwnd) && getControlText(form.field("prompt")).length > prompt.length,
        "Enter in prompt saved the form instead of inserting a newline",
      );
      const popup = findTopWindow(session.proc.pid!, "SumatraTranslatePopup");
      const labels: string[] = [];
      const footer: number[] = [];
      const header: number[] = [];
      enumChildWindows(popup, (child) => {
        if (getClassName(child).toLowerCase() !== "button") return true;
        labels.push(getControlText(child));
        const id = getWindowLong(child, GWL_ID);
        if (id >= 3001 && id <= 3005) footer.push(id);
        if (Object.values(ReadingControl).includes(id as (typeof ReadingControl)[keyof typeof ReadingControl]))
          header.push(id);
        return true;
      });
      assert(
        footer.sort((a, b) => a - b).join(",") === "3001,3002,3003,3004,3005",
        "translation popup footer actions changed",
      );
      assert(
        header.sort((a, b) => a - b).join(",") === "3010,3011,3012,3013",
        "reading controls are missing or duplicated",
      );
      assert(
        labels.every((label) => !/询问|Ask AI/i.test(label)),
        "translation popup gained an Ask AI action",
      );
      assert(received.length === 0, "blank configuration sent a request");
      repaintWindow(form.hwnd);
      captureWindowToPng(form.hwnd, tmpPath("translate-options-defaults.png"));
      sendMessage(form.field("cancel"), BM_CLICK, 0, 0);
      await waitClosed(form.hwnd);
    });

    await run("custom-overrides-request", async ({ session, received, endpoint }) => {
      const form = await openConfig(session);
      fillConnection(form, endpoint);
      sendText(form.field("temperature"), "0.4");
      sendText(form.field("prompt"), VARIABLE_PROMPT);
      selectCombo(form.field("source"), /英语|English/i);
      selectCombo(form.field("target"), /日语|日本語|Japanese/i);
      selectCombo(form.field("thinking"), /^Low\b/);
      const custom = await openParams(session, form);
      fillParams(custom, [
        ["reasoning", '{"effort":"none"}'],
        ["model", '"override-model"'],
        ["temperature", "0.7"],
        ["stream", "false"],
        ["zero", "0"],
        ["disabled", "false"],
        ["optional", "null"],
        ["nested", '{"items":[0,false,null,"${langTo}",{"escaped":"\\u4e2d"}]}'],
      ]);
      repaintWindow(custom);
      captureWindowToPng(custom, tmpPath("translate-options-custom-rows.png"));
      sendMessage(controlById(custom, ParamControl.save), BM_CLICK, 0, 0);
      await waitClosed(custom);
      sendMessage(form.field("save"), BM_CLICK, 0, 0);
      await waitClosed(form.hwnd);
      const finished = await expectText(session);
      assert(received.length === 1, "settings Save sent more than one request");
      const { body, raw } = received[0]!;
      assert(
        body.model === "override-model" && body.temperature === 0.7 && body.stream === false,
        "custom parameters did not override standard fields",
      );
      assert(
        (raw.match(/"model"\s*:/g) ?? []).length === 1 && (raw.match(/"temperature"\s*:/g) ?? []).length === 1,
        "overrides serialized duplicate standard keys",
      );
      assert(
        body.messages?.length === 2 && body.messages[1].content === "English|Japanese|quick",
        "language or prompt options did not reach the request",
      );
      assert(
        body.messages[0].content.includes("Japanese") && !body.messages[0].content.includes("quick"),
        "selected text entered system instructions",
      );
      assert(
        body.reasoning?.effort === "none" && body.zero === 0 && body.disabled === false && body.optional === null,
        "custom primitive JSON values changed",
      );
      assert(
        JSON.stringify(body.nested) === JSON.stringify({ items: [0, false, null, "${langTo}", { escaped: "中" }] }),
        "nested custom JSON changed",
      );
      assert(/日语|日本語|Japanese/.test(finished.status), "status still shows the old target language");
    });

    await run("custom-and-main-cancel", async ({ session, received, endpoint }) => {
      let form = await openConfig(session);
      fillConnection(form, endpoint);
      let custom = await openParams(session, form);
      fillParams(custom, [["discarded", "false"]]);
      sendMessage(controlById(custom, ParamControl.cancel), BM_CLICK, 0, 0);
      await waitClosed(custom);
      custom = await openParams(session, form);
      assert(
        paramsValues(custom).every(([name, value]) => !name && !value),
        "custom Cancel kept its edits",
      );
      fillParams(custom, [["draft", '{"value":1}']]);
      sendMessage(controlById(custom, ParamControl.save), BM_CLICK, 0, 0);
      await waitClosed(custom);
      sendMessage(form.field("cancel"), BM_CLICK, 0, 0);
      await waitClosed(form.hwnd);
      form = await openConfig(session);
      assert(getControlText(form.field("url")) === "", "main Cancel saved the endpoint draft");
      custom = await openParams(session, form);
      assert(
        paramsValues(custom).every(([name, value]) => !name && !value),
        "main Cancel saved custom parameter drafts",
      );
      assert(received.length === 0, "cancelled settings triggered an API request");
      sendMessage(custom, WM_CLOSE, 0, 0);
      sendMessage(form.field("cancel"), BM_CLICK, 0, 0);
    });

    await run("invalid-settings-and-json", async ({ session, received, endpoint }) => {
      const form = await openConfig(session);
      fillConnection(form, endpoint);
      for (const value of ["NaN", "-0.1", "2.1", "0.3oops"]) {
        sendText(form.field("temperature"), value);
        sendMessage(form.field("save"), BM_CLICK, 0, 0);
        assert(isWindowVisible(form.hwnd), "invalid temperature was accepted");
      }
      sendText(form.field("temperature"), "0.5");
      sendText(form.field("prompt"), "Translate this text with no source variable");
      sendMessage(form.field("save"), BM_CLICK, 0, 0);
      assert(isWindowVisible(form.hwnd), "prompt without sourceText was accepted");
      sendText(form.field("prompt"), VARIABLE_PROMPT);
      const custom = await openParams(session, form);
      fillParams(custom, [["reasoning", "{"]]);
      sendMessage(controlById(custom, ParamControl.save), BM_CLICK, 0, 0);
      assert(isWindowVisible(custom), "malformed JSON was accepted");
      sendText(controlById(custom, 3301), "false");
      sendMessage(controlById(custom, ParamControl.add), BM_CLICK, 0, 0);
      sendText(controlById(custom, 3303), "reasoning");
      sendText(controlById(custom, 3304), "0");
      sendMessage(controlById(custom, ParamControl.save), BM_CLICK, 0, 0);
      assert(isWindowVisible(custom), "duplicate custom names were accepted");
      sendText(controlById(custom, 3303), "");
      sendMessage(controlById(custom, ParamControl.save), BM_CLICK, 0, 0);
      assert(isWindowVisible(custom), "JSON value without a parameter name was accepted");
      assert(received.length === 0, "invalid settings made an API request");
      sendMessage(controlById(custom, ParamControl.cancel), BM_CLICK, 0, 0);
      sendMessage(form.field("cancel"), BM_CLICK, 0, 0);
    });

    await run(
      "persist-and-reload-options",
      async ({ session, received, endpoint, appdata }) => {
        const form = await openConfig(session);
        const prompt = "Translate ${langFrom} to ${langTo}.\r\nOnly the result:\r\n${sourceText}";
        fillConnection(form, endpoint);
        sendText(form.field("temperature"), "0.35");
        sendText(form.field("prompt"), prompt);
        sendMessage(form.field("stream"), BM_SETCHECK, 0, 0);
        selectCombo(form.field("source"), /英语|English/i);
        selectCombo(form.field("target"), /日语|日本語|Japanese/i);
        selectCombo(form.field("thinking"), /^High\b/);
        const custom = await openParams(session, form);
        fillParams(custom, [
          ["max_tokens", "321"],
          ["metadata", '{"keep":false,"zero":0}'],
        ]);
        sendMessage(controlById(custom, ParamControl.save), BM_CLICK, 0, 0);
        await waitClosed(custom);
        sendMessage(form.field("save"), BM_CLICK, 0, 0);
        await waitClosed(form.hwnd);
        await expectText(session);
        const settingsPath = join(appdata, "SumatraPDF-settings.txt");
        await pollUntil(
          () => (existsSync(settingsPath) ? readFileSync(settingsPath, "utf8") : ""),
          (text) =>
            /TranslatePopupLangFrom\s*=\s*en\b/.test(text) &&
            /TranslatePopupLangTo\s*=\s*ja\b/.test(text) &&
            /TranslatePopupThinking\s*=\s*high\b/.test(text),
          { timeoutMs: TIMEOUT, error: "language and thinking options were not persisted" },
        );
        await quit(session);
        const before = received.length;
        const restarted = await launchControlled(["-appdata", appdata, pdf]);
        try {
          await restarted.client.waitForRenderIdle();
          await restarted.client.setNotificationsEnabled(false);
          await drag(restarted);
          await expectText(restarted);
          const request = received[before]?.body;
          assert(received.length === before + 1 && request, "reloaded options did not trigger exactly one request");
          assert(
            request.temperature === 0.35 && request.stream === false && request.max_tokens === 321,
            "temperature, stream or custom options failed to reload",
          );
          assert(
            request.messages[1].content.replace(/\r\n/g, "\n") ===
              "Translate English to Japanese.\nOnly the result:\nquick",
            "multiline template or language selections failed to reload",
          );
          const reloaded = await openConfig(restarted);
          assert(
            /^High\b/.test(getControlText(reloaded.field("thinking"))) &&
              getControlText(reloaded.field("prompt")).replace(/\r\n/g, "\n") === prompt.replace(/\r\n/g, "\n"),
            "thinking or prompt form values failed to reload",
          );
          const reloadedParams = await openParams(restarted, reloaded);
          const values = Object.fromEntries(paramsValues(reloadedParams).filter(([name]) => !!name));
          assert(
            JSON.parse(values.metadata!)?.keep === false && JSON.parse(values.metadata!)?.zero === 0,
            "custom JSON failed to reload",
          );
          sendMessage(reloadedParams, WM_CLOSE, 0, 0);
          sendMessage(reloaded.field("cancel"), BM_CLICK, 0, 0);
          writeFileSync(
            tmpPath("translate-options-persistence.json"),
            JSON.stringify(
              {
                savedAndReloaded: true,
                temperature: true,
                stream: true,
                prompt: true,
                languages: true,
                thinking: true,
                customJson: true,
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
      "responses-json-nonstream",
      async ({ session, received }) => {
        await expectText(session, "中文 α≤β 😀");
        const body = received[0]?.body;
        assert(body?.stream === false && body.temperature === 0.6, "Responses options did not reach request");
        assert(
          body.input === "English|Simplified Chinese|quick" && typeof body.instructions === "string" && !body.messages,
          "Responses used Chat Completions request fields",
        );
        assert(!body.instructions.includes("quick"), "Responses selected text entered instructions");
      },
      {
        configured: true,
        responses: true,
        settings: [
          "TranslatePopupStream = false",
          "TranslatePopupTemperature = 0.6",
          "TranslatePopupLangFrom = en",
          `TranslatePopupPrompt = ${VARIABLE_PROMPT}`,
        ],
        reply: () =>
          Response.json({
            status: "completed",
            output: [
              { type: "reasoning", content: [{ type: "reasoning_text", text: "do not show this" }] },
              ...responseOutput("中文 α≤β 😀"),
            ],
          }),
      },
    );

    let releaseDelta!: () => void;
    let releaseCompleted!: () => void;
    let deltaReleased = false;
    let completedReleased = false;
    const deltaGate = new Promise<void>((resolve) => {
      releaseDelta = () => {
        deltaReleased = true;
        resolve();
      };
    });
    const completedGate = new Promise<void>((resolve) => {
      releaseCompleted = () => {
        completedReleased = true;
        resolve();
      };
    });
    try {
      await run(
        "responses-stream-barriers",
        async ({ session, received }) => {
          await pollUntil(
            () => state(session.client),
            (value) => value.visible && value.busy && value.text === "译",
            { timeoutMs: TIMEOUT, error: "Responses first token was buffered until EOF" },
          );
          assert(!deltaReleased, "server released later tokens before first-token observation");
          releaseDelta();
          await pollUntil(
            () => state(session.client),
            (value) => value.visible && value.busy && value.text === "译文😀",
            { timeoutMs: TIMEOUT, error: "output_text.done was duplicated or treated as terminal" },
          );
          assert(!completedReleased, "server completed before the done snapshot was checked");
          releaseCompleted();
          await expectText(session, "译文😀");
          assert(received[0]?.body.stream === true, "Responses Stream on was not sent");
        },
        {
          configured: true,
          responses: true,
          reply: () =>
            stream([
              {
                bytes:
                  event("response.reasoning_text.delta", { delta: "hidden chain of thought" }) +
                  event("response.output_text.delta", {
                    item_id: "m1",
                    output_index: 0,
                    content_index: 0,
                    delta: "译",
                  }),
              },
              {
                bytes:
                  event("response.output_text.delta", {
                    item_id: "m1",
                    output_index: 0,
                    content_index: 0,
                    delta: "文😀",
                  }) +
                  event("response.output_text.done", {
                    item_id: "m1",
                    output_index: 0,
                    content_index: 0,
                    text: "译文😀",
                  }),
                wait: deltaGate,
              },
              {
                bytes: event("response.completed", {
                  response: { status: "completed", output: responseOutput("译文😀") },
                }),
                wait: completedGate,
              },
            ]),
        },
      );
    } finally {
      releaseDelta();
      releaseCompleted();
    }

    await run(
      "responses-done-snapshot",
      async ({ session }) => {
        await expectText(session, "仅最终快照😀");
      },
      {
        configured: true,
        responses: true,
        reply: () =>
          stream([
            {
              bytes:
                event("response.output_text.done", {
                  item_id: "m1",
                  output_index: 0,
                  content_index: 0,
                  text: "仅最终快照😀",
                }) +
                event("response.completed", {
                  response: { status: "completed", output: responseOutput("仅最终快照😀") },
                }),
            },
          ]),
      },
    );

    for (const terminal of ["failed", "incomplete"] as const) {
      await run(
        `responses-${terminal}`,
        async ({ session }) => {
          await expectError(session);
        },
        {
          configured: true,
          responses: true,
          reply: () =>
            stream([
              {
                bytes:
                  event("response.output_text.delta", {
                    item_id: "m1",
                    output_index: 0,
                    content_index: 0,
                    delta: "部分文本",
                  }) +
                  event(`response.${terminal}`, {
                    response: {
                      status: terminal,
                      error: terminal === "failed" ? { message: "mock request failed" } : null,
                      incomplete_details: terminal === "incomplete" ? { reason: "max_output_tokens" } : null,
                      output: responseOutput("部分文本"),
                    },
                  }),
              },
            ]),
        },
      );
    }

    await run(
      "responses-no-terminal",
      async ({ session }) => {
        await expectError(session);
      },
      {
        configured: true,
        responses: true,
        reply: () =>
          stream([
            {
              bytes:
                event("response.output_text.delta", {
                  item_id: "m1",
                  output_index: 0,
                  content_index: 0,
                  delta: "未完成",
                }) +
                event("response.output_text.done", {
                  item_id: "m1",
                  output_index: 0,
                  content_index: 0,
                  text: "未完成",
                }),
            },
          ]),
      },
    );

    await run(
      "one-pass-template",
      async ({ session, received }) => {
        await expectText(session);
        await drag(session, "${langTo}");
        await expectText(session);
        const last = received.at(-1)?.body;
        assert(
          last.messages[1].content === "Auto (detect the source language)|Japanese|${langTo}",
          "source text containing a template variable was substituted again",
        );
      },
      { configured: true, settings: ["TranslatePopupLangTo = ja", `TranslatePopupPrompt = ${VARIABLE_PROMPT}`] },
    );
  } finally {
    server.stop(true);
  }
  assert(results.length > 0, "no matching options scenario ran");
  assert(!results.some((item) => item.status === "FAIL"), "one or more native options checks failed");
}

if (import.meta.main) await runStandalone(testit);
