/* License: GPL-3.0-only */

#include "base/Base.h"
#include "base/Http.h"
#include "base/JsonParser.h"
#include "LlmRequestOptions.h"
#include "LlmTranslate.h"

constexpr int kLlmResponseLimit = 8 * 1024 * 1024;
constexpr int kLlmEventLimit = 1024 * 1024;
constexpr int kLlmTextLimit = 2 * 1024 * 1024;
constexpr int kLlmKeyLimit = 4096;
constexpr int kLlmModelLimit = 1024;
constexpr int kLlmUrlLimit = 8192;
constexpr int kLlmErrorRunes = 1024;
constexpr int kLlmHttpOk = 200;
constexpr int kLlmHttpRedirect = 300;

enum class LlmWireMode {
    Unknown,
    Json,
    Sse,
};

enum class LlmReplyKind {
    Json,
    Event,
    Error,
};

enum class LlmProtocol {
    Chat,
    Responses,
};

struct LlmTextPart {
    int output = 0;
    int content = 0;
    Str text;
};

struct LlmOutputInfo {
    int index = 0;
    Str type;
    Str role;
};

struct LlmSnapshotPart {
    int output = 0;
    int content = 0;
    Str type;
    Str text;
    bool hasText = false;
};

static LlmOutputInfo* LlmFindOutput(Vec<LlmOutputInfo>& outputs, int index) {
    for (auto& output : outputs) {
        if (output.index == index) return &output;
    }
    if (index < 0 || index > 1024 || len(outputs) >= 1024) return nullptr;
    VecAppend(outputs, LlmOutputInfo{index});
    return &outputs[len(outputs) - 1];
}

static LlmSnapshotPart* LlmFindSnapshot(Vec<LlmSnapshotPart>& parts, int output, int content) {
    for (auto& part : parts) {
        if (part.output == output && part.content == content) return &part;
    }
    if (output < 0 || content < 0 || output > 1024 || content > 1024 || len(parts) >= 1024) return nullptr;
    VecAppend(parts, LlmSnapshotPart{output, content});
    return &parts[len(parts) - 1];
}

static int LlmEventIndex(Str number) {
    if (len(number) == 0 || len(number) > 4) return -1;
    int index = 0;
    for (int i = 0; i < len(number); i++) {
        char c = number.s[i];
        if (c < '0' || c > '9') return -1;
        index = index * 10 + c - '0';
    }
    return index <= 1024 ? index : -1;
}

static bool LlmValidUtf8(Str text) {
    const u8* at = (const u8*)text.s;
    return len(text) == 0 || isLegalUTF8String(&at, at + len(text));
}

// Reader encodes escaped UTF-16 units separately. Wire UTF-8 is validated
// before parsing, so only JSON escapes can produce the surrogate bytes here.
static bool LlmNormalizeJsonText(Str text, str::Builder& out) {
    constexpr int kCesuPairBytes = 6;
    constexpr u8 kCesuLead = 0xed;
    constexpr u8 kHighSecond = 0xa0;
    constexpr u8 kLowSecond = 0xb0;
    constexpr u8 kSecondMax = 0xbf;
    constexpr u8 kTailMin = 0x80;
    constexpr u8 kTailMax = 0xbf;
    constexpr int kSupplementStart = 0x10000;
    const u8* bytes = (const u8*)text.s;
    for (int i = 0; i < len(text);) {
        if (bytes[i] != kCesuLead || i + 1 >= len(text) || bytes[i + 1] < kHighSecond || bytes[i + 1] > kSecondMax) {
            out.AppendChar(text.s[i++]);
            continue;
        }
        if (len(text) - i < kCesuPairBytes || bytes[i + 1] >= kLowSecond || bytes[i + 2] < kTailMin ||
            bytes[i + 2] > kTailMax || bytes[i + 3] != kCesuLead || bytes[i + 4] < kLowSecond ||
            bytes[i + 4] > kSecondMax || bytes[i + 5] < kTailMin || bytes[i + 5] > kTailMax) {
            return false;
        }
        int high = ((bytes[i + 1] & 0x0f) << 6) | (bytes[i + 2] & 0x3f);
        int low = ((bytes[i + 4] & 0x0f) << 6) | (bytes[i + 5] & 0x3f);
        int scalar = kSupplementStart + (high << 10) + low;
        char utf8[4];
        int n = 0;
        str::Utf8Encode(utf8, n, scalar);
        out.Append(Str(utf8, n));
        i += kCesuPairBytes;
    }
    return LlmValidUtf8(ToStr(out));
}

static bool LlmValidUrl(Str url) {
    int prefix = str::StartsWithI(url, StrL("https://")) ? LenL("https://") : 0;
    if (prefix == 0 && str::StartsWithI(url, StrL("http://"))) {
        prefix = LenL("http://");
    }
    if (prefix == 0 || len(url) <= prefix || len(url) > kLlmUrlLimit || !LlmValidUtf8(url)) {
        return false;
    }
    for (int i = 0; i < len(url); i++) {
        u8 c = (u8)url.s[i];
        if (c <= ' ' || c == 0x7f || c == '#') {
            return false;
        }
    }
    int end = prefix;
    while (end < len(url) && url.s[end] != '/' && url.s[end] != '?') {
        if (url.s[end] == '@') {
            return false;
        }
        end++;
    }
    return end > prefix;
}

static bool LlmValidKey(Str key) {
    if (len(key) > kLlmKeyLimit) {
        return false;
    }
    for (int i = 0; i < len(key); i++) {
        u8 c = (u8)key.s[i];
        if (c < ' ' || c > '~') {
            return false;
        }
    }
    return true;
}

struct LlmReplySink : HttpStreamSink {
    HttpStreamCancel* cancel;
    LlmTranslateProgress* progress;
    Str apiKey;
    LlmProtocol protocol = LlmProtocol::Chat;
    LlmWireMode mode = LlmWireMode::Unknown;
    int status = 0;
    int received = 0;
    int textBytes = 0;
    bool done = false;
    bool afterCR = false;
    bool errorEvent = false;
    str::Builder pending;
    str::Builder line;
    str::Builder event;
    str::Builder response;
    str::Builder errorBody;
    Vec<LlmTextPart> textParts;
    Str error;

    LlmReplySink(HttpStreamCancel* cancel, LlmTranslateProgress* progress, Str apiKey,
                 LlmProtocol protocol = LlmProtocol::Chat)
        : cancel(cancel), progress(progress), apiKey(apiKey), protocol(protocol) {}

    ~LlmReplySink() override {
        str::Free(error);
        for (auto& part : textParts) str::Free(part.text);
    }

    void Fail(Str message) {
        if (len(error) > 0) {
            return;
        }
        TempStr safe = len(apiKey) > 0 ? str::ReplaceTemp(message, apiKey, StrL("[redacted]")) : message;
        error = str::Dup(ShortenStringUtf8Temp(safe, kLlmErrorRunes));
    }

    bool IsCanceled() override { return cancel->IsCanceled(); }

    void OnStatus(int code) override { status = code; }

    bool SetTextPart(int output, int content, Str text, LlmReplyKind kind) {
        if (output < 0 || content < 0 || output > 1024 || content > 1024) {
            Fail(StrL("翻译服务返回了无效的文本位置。"));
            return false;
        }
        str::Builder normalized;
        if (!LlmNormalizeJsonText(text, normalized) || str::ContainsChar(ToStr(normalized), '\0')) {
            Fail(StrL("翻译服务返回了无效的文本。"));
            return false;
        }
        LlmTextPart* part = nullptr;
        for (auto& candidate : textParts) {
            if (candidate.output == output && candidate.content == content) {
                part = &candidate;
                break;
            }
        }
        if (!part) {
            if (len(textParts) >= 1024) {
                Fail(StrL("翻译服务返回的文本片段过多。"));
                return false;
            }
            VecAppend(textParts, LlmTextPart{output, content});
            part = &textParts[len(textParts) - 1];
        }
        int oldSize = len(part->text);
        int size = len(normalized) + (kind == LlmReplyKind::Event ? oldSize : 0);
        if (size > kLlmTextLimit - (textBytes - oldSize)) {
            Fail(StrL("译文过长，已停止接收。"));
            return false;
        }
        if (kind == LlmReplyKind::Event) {
            str::Builder appended;
            appended.Append(part->text);
            appended.Append(ToStr(normalized));
            str::Free(part->text);
            part->text = appended.TakeStr();
        } else {
            str::ReplaceWithCopy(&part->text, ToStr(normalized));
        }
        textBytes += size - oldSize;
        return true;
    }

    void PublishParts() {
        VecSort(textParts, [](const LlmTextPart* a, const LlmTextPart* b) {
            if (a->output != b->output) return a->output - b->output;
            return a->content - b->content;
        });
        str::Builder next;
        for (const auto& part : textParts) next.Append(part.text);
        if (str::Eq(ToStr(next), ToStr(response))) return;
        response.Reset(ToStr(next));
        if (progress && !IsCanceled()) progress->OnText(ToStr(response));
    }

    void ParseResponses(Str data, LlmReplyKind kind) {
        Str type;
        Str responseStatus;
        Str delta;
        Str finalText;
        bool hasDelta = false;
        bool hasFinalText = false;
        Str failure;
        Str incomplete;
        int outputIndex = 0;
        int contentIndex = 0;
        Vec<LlmOutputInfo> outputs;
        Vec<LlmSnapshotPart> parts;
        json::Reader reader(data);
        for (json::Value& value : reader) {
            StrNode* path = value.path;
            if (json::PathMatch(path, StrL("/output_index"))) {
                outputIndex = value.type == json::Type::Number ? LlmEventIndex(value.value) : -1;
            }
            if (json::PathMatch(path, StrL("/content_index"))) {
                contentIndex = value.type == json::Type::Number ? LlmEventIndex(value.value) : -1;
            }
            if (value.type == json::Type::Number) {
                continue;
            }
            if (value.type != json::Type::String) continue;
            Str text = str::DupTemp(value.value);
            if (json::PathMatch(path, StrL("/type"))) type = text;
            if (json::PathMatch(path, StrL("/delta"))) {
                delta = text;
                hasDelta = true;
            }
            if (json::PathMatch(path, StrL("/text"))) {
                finalText = text;
                hasFinalText = true;
            }
            if (path && str::Eq(path->s, StrL("/response"))) path = path->next;
            if (json::PathMatch(path, StrL("/status"))) responseStatus = text;
            if (json::PathMatch(path, StrL("/error"), StrL("/message")) || json::PathMatch(path, StrL("/error")) ||
                json::PathMatch(path, StrL("/message")))
                failure = text;
            if (json::PathMatch(path, StrL("/incomplete_details"), StrL("/reason"))) incomplete = text;
            if (json::PathMatch(path, StrL("/output"), StrL("*"), StrL("/type"))) {
                int index = json::PathSegIndex(json::PathNth(path, 1));
                auto* output = LlmFindOutput(outputs, index);
                if (!output) {
                    Fail(StrL("翻译服务返回的文本片段过多。"));
                    return;
                }
                output->type = text;
            }
            if (json::PathMatch(path, StrL("/output"), StrL("*"), StrL("/role"))) {
                int index = json::PathSegIndex(json::PathNth(path, 1));
                auto* output = LlmFindOutput(outputs, index);
                if (!output) {
                    Fail(StrL("翻译服务返回的文本片段过多。"));
                    return;
                }
                output->role = text;
            }
            bool partType =
                json::PathMatch(path, StrL("/output"), StrL("*"), StrL("/content"), StrL("*"), StrL("/type"));
            bool partText =
                json::PathMatch(path, StrL("/output"), StrL("*"), StrL("/content"), StrL("*"), StrL("/text"));
            if (!partType && !partText) continue;
            int output = json::PathSegIndex(json::PathNth(path, 1));
            int content = json::PathSegIndex(json::PathNth(path, 3));
            auto* part = LlmFindSnapshot(parts, output, content);
            if (!part) {
                Fail(StrL("翻译服务返回的文本片段过多。"));
                return;
            }
            if (partType) part->type = text;
            if (partText) {
                part->text = text;
                part->hasText = true;
            }
        }
        if (!reader.Ok()) {
            Fail(StrL("翻译服务返回了无效的 JSON。"));
            return;
        }
        if (len(failure)) {
            Fail(fmt("翻译服务错误：%s", failure));
            return;
        }
        if (kind == LlmReplyKind::Error || str::Eq(type, StrL("error")) || str::Eq(type, StrL("response.failed")) ||
            str::Eq(responseStatus, StrL("failed"))) {
            Fail(StrL("翻译服务报告了错误。"));
            return;
        }
        if (str::Eq(type, StrL("response.incomplete")) || str::Eq(responseStatus, StrL("incomplete")) ||
            str::Eq(responseStatus, StrL("cancelled")) ||
            ((kind == LlmReplyKind::Json || str::Eq(type, StrL("response.completed"))) && len(responseStatus) &&
             !str::Eq(responseStatus, StrL("completed")))) {
            Fail(len(incomplete) ? fmt("翻译未完成：%s", incomplete)
                                 : StrL("翻译服务未完成译文，请缩短选区或调整输出上限。"));
            return;
        }
        if (kind == LlmReplyKind::Event && str::Eq(type, StrL("response.output_text.delta"))) {
            if (!hasDelta) {
                Fail(StrL("翻译服务返回了无效的文本事件。"));
                return;
            }
            if (SetTextPart(outputIndex, contentIndex, delta, LlmReplyKind::Event)) PublishParts();
            return;
        }
        if (kind == LlmReplyKind::Event && str::Eq(type, StrL("response.output_text.done"))) {
            if (!hasFinalText) {
                Fail(StrL("翻译服务返回了无效的文本事件。"));
                return;
            }
            if (SetTextPart(outputIndex, contentIndex, finalText, LlmReplyKind::Json)) PublishParts();
            return;
        }
        bool completed = str::Eq(type, StrL("response.completed"));
        if (kind != LlmReplyKind::Json && !completed) return;
        for (const auto& part : parts) {
            const auto* output = LlmFindOutput(outputs, part.output);
            if (!output || !str::Eq(output->type, StrL("message")) ||
                (len(output->role) && !str::Eq(output->role, StrL("assistant"))) ||
                !str::Eq(part.type, StrL("output_text")) || !part.hasText)
                continue;
            if (!SetTextPart(part.output, part.content, part.text, LlmReplyKind::Json)) return;
        }
        PublishParts();
        done = true;
    }

    void ParseReply(Str data, LlmReplyKind kind) {
        str::TrimWsBoth(data);
        if (len(data) == 0 || done || len(error) > 0) {
            return;
        }
        if (kind == LlmReplyKind::Error && str::Eq(data, StrL("[DONE]"))) {
            Fail(StrL("翻译服务报告了错误。"));
            return;
        }
        if (kind != LlmReplyKind::Json && str::Eq(data, StrL("[DONE]"))) {
            if (protocol == LlmProtocol::Responses)
                Fail(StrL("翻译流未包含完成事件，译文可能不完整。"));
            else
                done = true;
            return;
        }
        if (!LlmValidUtf8(data)) {
            Fail(StrL("翻译服务返回了无效的 UTF-8 文本。"));
            return;
        }
        if (protocol == LlmProtocol::Responses) {
            ParseResponses(data, kind);
            return;
        }

        json::Reader reader(data);
        str::Builder text;
        Str failure;
        Str finish;
        for (json::Value& value : reader) {
            if (value.type != json::Type::String) {
                continue;
            }
            bool delta = json::PathMatch(value.path, StrL("/choices"), StrL("i0"), StrL("/delta"), StrL("/content"));
            bool message =
                json::PathMatch(value.path, StrL("/choices"), StrL("i0"), StrL("/message"), StrL("/content"));
            if ((kind == LlmReplyKind::Json && message) || (kind == LlmReplyKind::Event && delta)) {
                str::Builder normalized;
                if (!LlmNormalizeJsonText(value.value, normalized)) {
                    Fail(StrL("翻译服务返回了无效的文本。"));
                    return;
                }
                text.Append(ToStr(normalized));
            }
            if (json::PathMatch(value.path, StrL("/error"), StrL("/message")) ||
                json::PathMatch(value.path, StrL("/error")) ||
                (kind == LlmReplyKind::Error && json::PathMatch(value.path, StrL("/message")))) {
                failure = str::DupTemp(value.value);
            }
            if (json::PathMatch(value.path, StrL("/choices"), StrL("i0"), StrL("/finish_reason"))) {
                finish = str::DupTemp(value.value);
            }
        }
        if (!reader.Ok()) {
            Fail(StrL("翻译服务返回了无效的 JSON。"));
            return;
        }
        if (len(failure) > 0) {
            Fail(fmt("翻译服务错误：%s", failure));
            return;
        }
        if (kind == LlmReplyKind::Error) {
            Fail(StrL("翻译服务报告了错误。"));
            return;
        }
        if (len(finish) > 0 && !str::Eq(finish, StrL("stop"))) {
            if (str::Eq(finish, StrL("length"))) {
                Fail(StrL("译文达到模型输出上限，内容不完整。请缩短选区或调整服务端输出上限。"));
            } else if (str::Eq(finish, StrL("content_filter"))) {
                Fail(StrL("翻译服务的内容过滤器终止了请求。"));
            } else {
                Fail(StrL("翻译服务未正常完成文本翻译。"));
            }
            return;
        }
        if (len(text) > kLlmTextLimit - len(response)) {
            Fail(StrL("译文过长，已停止接收。"));
            return;
        }
        Str addition = ToStr(text);
        if (str::ContainsChar(addition, '\0') || !LlmValidUtf8(addition)) {
            Fail(StrL("翻译服务返回了无效的文本。"));
            return;
        }
        if (len(addition) > 0) {
            response.Append(addition);
            if (progress && !IsCanceled()) {
                progress->OnText(ToStr(response));
            }
        }
        done = kind == LlmReplyKind::Json || str::Eq(finish, StrL("stop"));
    }

    void ParseLine() {
        Str value = ToStr(line);
        if (len(value) == 0) {
            ParseReply(ToStr(event), errorEvent ? LlmReplyKind::Error : LlmReplyKind::Event);
            event.Reset();
            errorEvent = false;
            return;
        }
        if (str::StartsWith(value, StrL("event:"))) {
            value = Str(value.s + LenL("event:"), len(value) - LenL("event:"));
            str::TrimWsBoth(value);
            errorEvent = str::Eq(value, StrL("error"));
            return;
        }
        if (!str::StartsWith(value, StrL("data:"))) {
            return;
        }
        value = Str(value.s + LenL("data:"), len(value) - LenL("data:"));
        if (len(value) > 0 && value.s[0] == ' ') {
            value = Str(value.s + 1, len(value) - 1);
        }
        if (len(event) > 0) {
            event.AppendChar('\n');
        }
        event.Append(value);
        if (len(event) > kLlmEventLimit) {
            Fail(StrL("翻译服务返回的单个事件过长。"));
        }
    }

    bool FeedSse(Str bytes) {
        for (int i = 0; i < len(bytes); i++) {
            char c = bytes.s[i];
            if (c == '\r' || c == '\n') {
                if (c == '\r' || !afterCR) {
                    ParseLine();
                    line.Reset();
                }
                afterCR = c == '\r';
            } else {
                afterCR = false;
                line.AppendChar(c);
            }
            if (len(line) > kLlmEventLimit) {
                Fail(StrL("翻译服务返回的单行过长。"));
            }
            if (done || len(error) > 0 || IsCanceled()) {
                return false;
            }
        }
        return true;
    }

    bool OnData(Str bytes) override {
        if (IsCanceled() || done || len(error) > 0) {
            return false;
        }
        if (len(bytes) > kLlmResponseLimit - received) {
            Fail(StrL("翻译服务响应过大，已停止接收。"));
            return false;
        }
        received += len(bytes);
        if (status < kLlmHttpOk || status >= kLlmHttpRedirect) {
            errorBody.Append(bytes);
            return true;
        }
        if (mode == LlmWireMode::Sse) {
            return FeedSse(bytes);
        }
        pending.Append(bytes);
        if (mode == LlmWireMode::Json) {
            return true;
        }

        Str prefix = ToStr(pending);
        if (len(prefix) > 0 && (u8)prefix.s[0] == 0xef) {
            if (len(prefix) < LenL(kUtf8Bom)) {
                return true;
            }
            str::TrimPrefix(prefix, StrL(kUtf8Bom));
        }
        str::TrimWs(prefix);
        if (len(prefix) == 0) {
            return true;
        }
        if (prefix.s[0] == '{' || prefix.s[0] == '[') {
            mode = LlmWireMode::Json;
            return true;
        }
        mode = LlmWireMode::Sse;
        bool keepReading = FeedSse(prefix);
        pending.Reset();
        return keepReading;
    }

    void Finish() {
        if (IsCanceled() || done || len(error) > 0) {
            return;
        }
        if (mode == LlmWireMode::Json) {
            Str data = ToStr(pending);
            str::TrimPrefix(data, StrL(kUtf8Bom));
            ParseReply(data, LlmReplyKind::Json);
            return;
        }
        if (mode == LlmWireMode::Sse) {
            if (len(line) > 0) {
                ParseLine();
                line.Reset();
            }
            ParseReply(ToStr(event), errorEvent ? LlmReplyKind::Error : LlmReplyKind::Event);
        }
    }

    void HttpError() {
        Str message;
        json::Reader reader(ToStr(errorBody));
        for (json::Value& value : reader) {
            if (value.type == json::Type::String &&
                (json::PathMatch(value.path, StrL("/error"), StrL("/message")) ||
                 json::PathMatch(value.path, StrL("/error")) || json::PathMatch(value.path, StrL("/message")))) {
                message = str::DupTemp(value.value);
            }
        }
        if (reader.Ok() && len(message) > 0 && LlmValidUtf8(message)) {
            Fail(fmt("HTTP %d：%s", status, message));
            return;
        }
        Fail(fmt("HTTP %d：翻译请求失败。请检查接口地址、密钥和模型。", status));
    }
};

// Runs on the caller's worker. The caller frees error; callback text is borrowed.
bool RunLlmTranslate(const LlmTranslateRequest& request, HttpStreamCancel* cancel, LlmTranslateProgress* progress,
                     Str& error) {
    error = {};
    if (!LlmValidUrl(request.url)) {
        error = str::Dup(StrL("请配置完整的 HTTP 或 HTTPS 翻译接口地址。"));
        return false;
    }
    if (str::IsEmptyOrWhiteSpace(request.model) || len(request.model) > kLlmModelLimit ||
        !LlmValidUtf8(request.model) || str::ContainsChar(request.model, '\0')) {
        error = str::Dup(StrL("请配置有效的模型名称。"));
        return false;
    }
    if (!LlmValidKey(request.apiKey)) {
        error = str::Dup(StrL("API 密钥包含无效字符或过长，请重新填写。"));
        return false;
    }
    if (str::IsEmptyOrWhiteSpace(request.text) || !LlmValidUtf8(request.text) ||
        str::ContainsChar(request.text, '\0')) {
        error = str::Dup(StrL("选区没有有效的文字。"));
        return false;
    }
    if (len(request.text) > kLlmSourceLimit) {
        error = str::Dup(StrL("选区过长，请分段翻译。"));
        return false;
    }

    HttpStreamCancel localCancel;
    LlmReplySink sink(cancel ? cancel : &localCancel, progress, request.apiKey,
                      IsLlmResponses(request.url) ? LlmProtocol::Responses : LlmProtocol::Chat);
    if (sink.IsCanceled()) {
        return false;
    }
    Str body;
    if (!BuildLlmRequest(request, body, error)) return false;
    str::Builder headers;
    headers.Append(StrL("Content-Type: application/json\r\nAccept: text/event-stream, application/json"));
    if (len(request.apiKey) > 0) {
        headers.Append(fmt("\r\nAuthorization: Bearer %s", request.apiKey));
    }
    bool transportOk = HttpPostStream(request.url, ToStr(headers), body, &sink, sink.cancel);
    str::Free(body);
    if (sink.IsCanceled()) {
        return false;
    }
    if (sink.status == 0) {
        sink.Fail(StrL("无法连接翻译服务。请检查接口地址和网络连接。"));
    } else if (sink.status < kLlmHttpOk || sink.status >= kLlmHttpRedirect) {
        sink.HttpError();
    } else {
        sink.Finish();
        if (!transportOk && (sink.mode == LlmWireMode::Json || !sink.done)) {
            sink.Fail(StrL("连接中断或超时，译文可能不完整。"));
        } else if (!sink.done) {
            sink.Fail(StrL("翻译流提前结束，译文不完整。"));
        } else if (str::IsEmptyOrWhiteSpace(ToStr(sink.response))) {
            sink.Fail(StrL("翻译服务未返回译文。"));
        }
    }
    error = str::Dup(sink.error);
    return len(error) == 0;
}
