# Translation configuration implementation plan

> **For implementation agent:** Use subagent-driven-development for implementation, spec review, and quality review. Do not commit.

**Goal:** Add the pictured native translation settings, language selection, and working Responses support. Defaults: Auto to Simplified Chinese, thinking Off, temperature 1, stream on. The translation popup continues to have five buttons and no Ask AI action.

**Architecture:** The UI persists a draft only on main Save and snapshots all request fields into the worker. A request-options helper validates templates and custom JSON, builds either Chat Completions or Responses bodies, and maps known provider thinking options. The existing transport and worker ownership stay intact.

**Tech stack:** Win32 C++, project Str/Vec, generated settings, Bun native-control tests, MSVC x64 static release.

## Shared interface

Extend `src/LlmTranslate.h` after its existing four request strings with:

```cpp
Str langFrom;
Str langTo;
Str prompt;
Str thinking;
Str customParams;
double temperature = 1.0;
bool stream = true;
```

Empty values use Auto, zh-CN, the built-in prompt, off, and {} respectively. Create `src/LlmRequestOptions.h/.cpp` with this public contract (Str outputs named error/body/object are caller-owned):

```cpp
struct LlmLanguage { Str code; Str label; Str name; };
struct LlmJsonParam { Str name; Str value; };
const LlmLanguage* LlmLanguages(int& count);
Str LlmLanguageLabel(Str code);
Str LlmDefaultPrompt();
bool CheckLlmPrompt(Str prompt, Str& error);
bool ReadLlmParams(Str object, Vec<LlmJsonParam>& out, Str& error);
bool WriteLlmParams(const Vec<LlmJsonParam>& params, Str& object, Str& error);
void FreeLlmParams(Vec<LlmJsonParam>& params);
bool IsLlmResponses(Str url);
bool BuildLlmRequest(const LlmTranslateRequest& request, Str& body, Str& error);
```

Language codes start with auto and zh-CN; target excludes auto. Thinking values: default/off/low/medium/high/max. Custom request properties override standard top-level properties. Preserve raw JSON values, including false, 0, null, nested objects/arrays and escaped strings; reject invalid JSON, blank names, and duplicate keys. Substitute prompt variables once; selected text remains user content. `${sourceText}` is required. Never include source text in system instructions.

## Task 1: Request construction and response protocols

Files: `src/LlmTranslate.h`, `src/LlmTranslate.cpp`, new `src/LlmRequestOptions.h/.cpp`.

1. Add request defaults, language catalog, template validation and one-pass expansion.
2. Add strict, bounded custom JSON property parsing/serialization and deterministic override merge.
3. Build Chat Completions messages or Responses input/instructions from the endpoint path. Send temperature and the effective stream flag.
4. Map Off to DeepSeek chat `thinking.type=disabled` and Responses `reasoning.effort=none`; OpenAI/Azure use documented reasoning fields. Unknown providers omit automatic thinking fields and use custom parameters. Validate known mode values.
5. Parse Responses output text for JSON and SSE, ignoring reasoning; avoid duplicate done snapshots. Completed is terminal, failed/incomplete are errors even with partial text. Preserve chat errors, cancellation, limits, redaction, and Unicode normalization.
6. Format changed C++ and run bounded protocol/request tests with local mocks. Verify invalid options fail before HTTP.

## Task 2: Native settings and draft persistence

Files: `src/TranslatePopup.cpp`, `cmd/gen-settings.ts`, `cmd/ng-gen-settings.ts`, generated original/ng settings; a separate custom-dialog module if useful.

1. Add settings fields Temperature=1, Prompt=built-in default, Stream=true, Thinking=off, LangFrom=auto, LangTo=zh-CN, CustomParams={} in both generators and serialization lists. Regenerate outputs.
2. Add API/model/masked-key, temperature, multiline prompt, Stream, Thinking, source and target selectors, custom request button, and enabled checkbox.
3. Preserve scrolling, fixed Save/Cancel footer, DPI/theme handling, keyboard traversal, and invalid-field feedback. Enter inserts a newline in the prompt and activates focused buttons. Custom editor supports name+JSON value rows, add/remove, Save/Cancel, validation and scrolling. Closing/cancelling main settings discards its custom draft.
4. Snapshot and free every new worker field. Show the selected target language in pending/completed status. No worker reads mutable settings.
5. Use stable control IDs: 3101 URL, 3102 model, 3103 key, 3104 temperature, 3105 prompt, 3106 stream, 3107 thinking, 3108 source, 3109 target, 3110 enabled, 3111 custom. Keep Save=3006 and Cancel=3007. Custom dialog class `SumatraTranslateParams`; Add=3201, Save=3202, Cancel=3203; row name=3300+3*i, JSON value=3301+3*i, remove=3302+3*i.

## Task 3: Focused acceptance and regression tests

Files: existing `tests/ad-hoc-llm-translate.ts`, `tests/ad-hoc-translate-settings.ts`, new `tests/ad-hoc-translate-options.ts`; isolated harness files under ignored `agent-tools/` if needed.

1. Use stable IDs, replacing assumptions about exactly three EDIT controls.
2. Verify default controls, edits, main/custom cancel, malformed JSON/prompt/temperature rejection, nested values and override precedence.
3. Save and restart with isolated appdata; verify persisted fields affect actual request bodies and status language.
4. Verify Chat/Responses with stream on and off, strict first-token barrier, done snapshots, reasoning exclusion, Unicode, failed/incomplete events, and cancellation. Unknown provider mapping and known DeepSeek/OpenAI mapping receive targeted request-builder checks.
5. Keep the headless clipboard SKIP separate. Preserve evidence under `out/validation/options/` before another runner wipes scratch.

## Task 4: Integration, review, and delivery

Files: build project/filter lists, `premake5.files.lua`, `tools/package-translation.py`, `docs/selection-translation.md`.

1. Add new source modules to project generation and packaging whitelist. Generate project files and format.
2. Build Debug, run only relevant tests, and fix failures. Obtain spec approval before independent quality review; resolve important issues and repeat affected checks.
3. Build Release: `tools/build-translation.ps1 -Configuration Release`. Run the focused release suites and save receipts/screenshots. Do not run full suites.
4. Update documentation and template configuration, including provider mapping and custom-override behavior. Package a new timestamped delivery; preserve the old one.
5. Launch the actual extracted portable ZIP with developer PATH removed, render PDF, open popup/settings, and verify new defaults. Provide binary/source links and state the live-model boundary.

## Verification commands

```powershell
. ./agent-tools/env.ps1
./agent-tools/build.ps1 -Mode -dbg
$env:SUMATRA_TEST_EXE = (Resolve-Path out/dbg64/SumatraPDF.exe).Path
agent-tools/bun.exe tests/ad-hoc-llm-translate.ts --no-build
agent-tools/bun.exe tests/ad-hoc-translate-settings.ts --no-build
agent-tools/bun.exe tests/ad-hoc-translate-options.ts --no-build
./tools/build-translation.ps1 -Configuration Release
python tools/package-translation.py --inventory
```

Expected: focused cases PASS, only documented headless clipboard SKIP, x64 PE output, valid CRC/hashes, patch applies to the fixed upstream baseline. No live service credentials are supplied; local mocks do not establish real-provider latency or translation quality.
