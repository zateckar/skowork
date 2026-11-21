// client/core-tools web.fetch (add-core-tools-and-mcp design D3): GET over HTTP or HTTPS, every request and redirect
// checked by the egress rules of client/policy-enforcement, never with the user's Windows credentials, readable text
// out (HTML through the in-house extractor, documents through the sandboxed extract worker), binary downloads into the
// quarantine folder. Results are untrusted, with the URL as provenance.
#pragma once

#include "platform/http.h"
#include "policy/core.h"
#include "runtime/tools.h"
#include "tools/files.h"

#include <functional>
#include <memory>
#include <string>

namespace wb::tools {

struct WebToolEnv {
    std::function<std::shared_ptr<const policy::EffectivePolicy>()> policy;
    std::string quarantine_dir;   // <data root>\quarantine
    Extractor extract;            // null: documents are saved to the quarantine instead
    HttpClient* client = nullptr; // null: a client of the tool's own
    // client/core-tools Web Fetch Question (add-web-fetch-question): one request to the `small` role without tools,
    // attributed to the calling Run; and that role's context window in tokens, 0 when the user has no `small` role.
    // Either unset: a question is not answered and the page text is returned with a note.
    std::function<Result<std::string>(const std::string& system, const std::string& user, runtime::ToolContext& ctx)> ask_small;
    std::function<long long()> small_context_window;
};

inline constexpr size_t kWebQuestionMaxChars = 1000;
// The page text sent with a question: what fits the small role's window after room for the prompt and the answer, at
// a conservative 2.4 bytes per token.
inline constexpr long long kWebQuestionReserveTokens = 4096;
size_t web_question_page_bytes(long long small_context_window);

runtime::ToolDef web_fetch_tool(WebToolEnv env);

inline constexpr long long kWebDefaultRawBytes = 256ll << 10;
inline constexpr long long kWebMaxRawBytes = 4ll << 20;

}  // namespace wb::tools
