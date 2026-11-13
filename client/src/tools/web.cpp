#include "tools/web.h"

#include "base/strings.h"
#include "base/time.h"
#include "base/url.h"
#include "policy/egress.h"
#include "tools/html.h"
#include "tools/quarantine.h"
#include "tools/text.h"

#include <algorithm>

namespace wb::tools {

using runtime::Provenance;
using runtime::Risk;
using runtime::ToolContext;
using runtime::ToolDef;
using runtime::ToolOutput;
using runtime::ToolResource;

namespace {

std::string media_type(const std::string& content_type) {
    return to_lower(trim(content_type.substr(0, content_type.find(';'))));
}

std::string document_kind_of(const std::string& type, const std::string& name) {
    if (type == "application/pdf") return "pdf";
    if (type == "application/vnd.openxmlformats-officedocument.wordprocessingml.document") return "docx";
    if (type == "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet") return "xlsx";
    if (type == "application/vnd.openxmlformats-officedocument.presentationml.presentation") return "pptx";
    return document_kind(name);
}

bool textual_type(const std::string& type) {
    if (type.rfind("text/", 0) == 0) return true;
    for (const char* t : {"json", "xml", "javascript", "ecmascript", "yaml", "csv", "x-www-form-urlencoded"})
        if (type.find(t) != std::string::npos) return true;
    return false;
}

bool html_type(const std::string& type, std::string_view body) {
    if (type == "text/html" || type == "application/xhtml+xml") return true;
    if (!type.empty() && type != "application/octet-stream") return false;
    std::string head = to_lower(body.substr(0, 512));
    head = trim(head);
    return head.rfind("<!doctype html", 0) == 0 || head.rfind("<html", 0) == 0;
}

// Cuts UTF-8 text at most `max` bytes long, at a character boundary.
std::string cut_utf8(std::string s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

std::string web_error(const Error& e, const std::string& url) {
    if (e.code == "policy_denied") return "web.fetch to " + url + " was denied by policy: " + e.message + ". No request was sent to that host.";
    if (e.code == "insecure_redirect") return "web.fetch stopped: " + e.message + ".";
    if (e.code == "too_large") return "The response from " + url + " is larger than " + std::to_string(kMaxFileBytes >> 20) + " MB and was not downloaded.";
    if (e.code == "cancelled" || e.code == "aborted") return "web.fetch was cancelled.";
    return "web.fetch of " + url + " failed (" + e.code + "): " + e.message;
}

}  // namespace

ToolDef web_fetch_tool(WebToolEnv env) {
    auto e = std::make_shared<WebToolEnv>(std::move(env));
    std::shared_ptr<HttpClient> own;
    if (!e->client) own = std::make_shared<HttpClient>(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) Skowork/1.0");
    ToolDef t;
    t.id = "web.fetch";
    t.description = "Fetch a web page or file over HTTP(S) with GET. format \"text\" (default) returns readable text: HTML as "
                    "structured plain text with links, documents (docx, xlsx, pptx, pdf) as extracted text. format \"raw\" "
                    "returns the body as text, up to max_bytes. Binary files are saved to the quarantine folder and only their "
                    "path is returned; they cannot be opened or run without the user's approval.";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"url", {{"type", "string"}, {"description", "http:// or https:// URL."}}},
                      {"format", {{"type", "string"}, {"enum", {"text", "raw"}}}},
                      {"max_bytes", {{"type", "integer"}, {"minimum", 1}, {"maximum", kWebMaxRawBytes}, {"description", "raw only; default 262144."}}}}},
                    {"required", {"url"}}};
    t.risk = Risk::read;
    t.provenance = Provenance{"web", "", "", false};
    t.resources_of = [](const json& a) { return std::vector<ToolResource>{{ToolResource::Kind::url, jstr(a, "url")}}; };
    t.run = [e, own](ToolContext& ctx, const json& a) -> ToolOutput {
        const std::string url = trim(jstr(a, "url"));
        const auto parsed = parse_url(url);
        if (!parsed || (parsed->scheme != "http" && parsed->scheme != "https") || parsed->host.empty())
            return ToolOutput::failure("url must be an absolute http:// or https:// URL.");
        const std::string format = jstr(a, "format", "text");
        if (format != "text" && format != "raw") return ToolOutput::failure("format must be \"text\" or \"raw\".");
        const auto p = e->policy ? e->policy() : nullptr;
        policy::EgressGuard guard([p] { return p; });

        std::string body;
        bool too_large = false;
        HttpRequest req;
        req.url = url;
        req.allow_plain_http = true;
        req.headers = {{"Accept", "text/html,application/xhtml+xml,text/plain;q=0.9,*/*;q=0.8"}};
        req.on_chunk = [&](std::string_view chunk) {
            if (ctx.cancelled()) return false;
            if (body.size() + chunk.size() > static_cast<size_t>(kMaxFileBytes)) {
                too_large = true;
                return false;
            }
            body.append(chunk);
            return true;
        };
        auto before_send = [&](HttpRequest&) {
            body.clear();  // a redirect's body is not the answer
        };
        std::string final_url = url;
        HttpClient& client = e->client ? *e->client : *own;
        auto r = policy::guarded_fetch(client, req, guard, 10, nullptr, before_send, &final_url);
        if (too_large) return ToolOutput::failure(web_error(make_error("too_large"), url));
        if (ctx.cancelled()) return ToolOutput::failure("web.fetch was cancelled.");
        if (!r) return ToolOutput::failure(web_error(r.error(), url));

        const std::string content_type = r->header("content-type");
        const std::string type = media_type(content_type);
        std::string head = "URL: " + final_url + "\nStatus: " + std::to_string(r->status) + "\n";
        if (!content_type.empty()) head += "Content-Type: " + content_type + "\n";
        ToolOutput out;
        out.ok = r->status >= 200 && r->status < 300;
        out.provenance = Provenance{"web", final_url, iso8601_utc_now(), false};

        const std::string name = download_name(r->header("content-disposition"), final_url);
        const std::string doc = document_kind_of(type, name);
        auto quarantine = [&](const std::string& why) {
            if (e->quarantine_dir.empty()) {
                out.ok = false;
                out.content = head + "\n" + why + " Downloads are not available (no quarantine folder).";
                return out;
            }
            auto saved = quarantine_save(e->quarantine_dir, name, body, final_url, url == final_url ? std::string{} : url);
            if (!saved) {
                out.ok = false;
                out.content = head + "\n" + why + " Saving it failed: " + saved.error().message;
                return out;
            }
            out.content = head + "\n" + why + " Saved " + std::to_string(saved->size) + " bytes to the quarantine folder: " +
                          saved->path + "\nThe file is marked as downloaded from the internet. The agent cannot open it, and "
                          "running it needs the user's approval; the user can move it out of the quarantine to use it.";
            return out;
        };

        if (!doc.empty() && format == "text") {
            if (!e->extract) return quarantine("The response is a " + doc + " document, and text extraction is not available.");
            auto text = e->extract(doc, body, ctx);
            if (!text) return quarantine("Text extraction of the " + doc + " document failed: " + text.error().message + ".");
            out.content = head + "\n" + *text;
            return out;
        }
        if (looks_binary(body) || (!textual_type(type) && !html_type(type, body) && !type.empty() && type != "application/octet-stream"))
            return quarantine("The response is a binary file (" + (type.empty() ? std::string("unknown type") : type) + ").");
        auto decoded = decode_text(body);
        if (!decoded) return quarantine("The response is a binary file.");

        if (format == "raw") {
            const long long max = std::clamp<long long>(jint(a, "max_bytes", kWebDefaultRawBytes), 1, kWebMaxRawBytes);
            const size_t total = decoded->text.size();
            std::string text = cut_utf8(std::move(decoded->text), static_cast<size_t>(max));
            if (text.size() < total) head += "Truncated: " + std::to_string(text.size()) + " of " + std::to_string(total) + " bytes\n";
            out.content = head + "\n" + text;
            return out;
        }
        if (html_type(type, body)) {
            HtmlText h = html_to_text(decoded->text, final_url);
            if (!h.title.empty()) head += "Title: " + h.title + "\n";
            out.content = head + "\n" + h.text;
            return out;
        }
        out.content = head + "\n" + decoded->text;
        return out;
    };
    return t;
}

}  // namespace wb::tools
