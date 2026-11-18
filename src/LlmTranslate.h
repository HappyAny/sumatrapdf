/* License: GPL-3.0-only */

struct HttpStreamCancel;

struct LlmTranslateRequest {
    Str url;
    Str model;
    Str apiKey;
    Str text;
    Str langFrom;
    Str langTo;
    Str prompt;
    Str thinking;
    Str customParams;
    double temperature = 1.0;
    bool stream = true;
};

struct LlmTranslateProgress {
    virtual ~LlmTranslateProgress() = default;
    virtual void OnText(Str cumulativeText) = 0;
};

bool RunLlmTranslate(const LlmTranslateRequest&, HttpStreamCancel*, LlmTranslateProgress*, Str& error);
