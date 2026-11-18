/* License: GPL-3.0-only */

struct LlmTranslateRequest;

struct LlmLanguage {
    Str code;
    Str label;
    Str name;
};

struct LlmJsonParam {
    Str name;
    Str value;
};

const LlmLanguage* LlmLanguages(int& count);
Str LlmLanguageLabel(Str code);
Str LlmDefaultPrompt();
bool CheckLlmPrompt(Str prompt, Str& error);
bool ReadLlmParams(Str object, Vec<LlmJsonParam>& out, Str& error);
bool WriteLlmParams(const Vec<LlmJsonParam>& params, Str& object, Str& error);
void FreeLlmParams(Vec<LlmJsonParam>& params);
bool IsLlmResponses(Str url);
bool BuildLlmRequest(const LlmTranslateRequest& request, Str& body, Str& error);
