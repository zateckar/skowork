#include "telemetry/record.h"

#include "base/redact.h"
#include "base/strings.h"
#include "base/url.h"

#include <algorithm>

namespace wb::telemetry {

const char* to_string(Level l) {
    switch (l) {
    case Level::off: return "off";
    case Level::metadata: return "metadata";
    case Level::content: return "content";
    }
    return "metadata";
}

std::optional<Level> parse_level(std::string_view s) {
    if (s == "off") return Level::off;
    if (s == "metadata") return Level::metadata;
    if (s == "content") return Level::content;
    return std::nullopt;
}

Redactor::Redactor(const std::vector<std::string>& patterns) {
    for (const auto& p : patterns) {
        auto compiled = p.empty() || p.size() > 1000 ? std::nullopt : Pattern::compile(p);
        if (compiled) patterns_.push_back(std::move(*compiled));
        else invalid_.push_back(p);
    }
}

std::string Redactor::apply(std::string_view text) const {
    std::string out = redact_secrets(text);
    long long budget = kRedactionBudget;
    for (const auto& re : patterns_) {
        auto r = re.replace(out, kRedacted, budget);
        // Out of budget: drop the text rather than risk sending what the pattern was meant to hide.
        if (!r) return std::string(kRedacted);
        out = std::move(*r);
    }
    return out;
}
std::string domain_of(std::string_view s) {
    // trim returns a std::string: keep it alive for the whole function, not as a temporary behind a string_view.
    const std::string trimmed = trim(s);
    s = trimmed;
    std::string host;
    if (s.find("://") != std::string_view::npos) {
        const auto u = parse_url(s);
        if (!u) return {};
        host = u->host;
    } else {
        size_t end = s.find_first_of("/?#");
        std::string_view h = s.substr(0, end);
        if (const size_t at = h.rfind('@'); at != std::string_view::npos) h = h.substr(at + 1);
        if (!h.empty() && h.front() == '[') {
            const size_t close = h.find(']');
            h = close == std::string_view::npos ? std::string_view{} : h.substr(1, close - 1);
        } else if (const size_t colon = h.find(':'); colon != std::string_view::npos) {
            h = h.substr(0, colon);
        }
        host = to_lower(h);
    }
    // A host name only: letters, digits, dots, hyphens (and IPv6 colons).
    if (host.empty() || host.size() > 253) return {};
    for (const char c : host)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == ':')) return {};
    return host;
}

namespace {

bool type_matches(Type t, const Value& v) {
    switch (t) {
    case Type::string:
    case Type::domain: return std::holds_alternative<std::string>(v);
    case Type::integer: return std::holds_alternative<long long>(v);
    case Type::number: return std::holds_alternative<double>(v) || std::holds_alternative<long long>(v);
    case Type::boolean: return std::holds_alternative<bool>(v);
    }
    return false;
}

// Cuts at `max` bytes without splitting a UTF-8 sequence.
std::string cut(std::string s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

}  // namespace

std::optional<Record> make_record(Event e, long long time_ms, std::span<const Field> fields, Level level, const Redactor& redactor,
                                  FilterStats* stats) {
    if (level == Level::off) return std::nullopt;
    FilterStats local;
    FilterStats& st = stats ? *stats : local;
    const EventDef& ev = def(e);
    Record r;
    r.event = e;
    r.time_ms = time_ms;
    for (const Field& f : fields) {
        if (std::find(ev.attrs.begin(), ev.attrs.end(), f.attr) == ev.attrs.end()) {
            ++st.not_in_event;
            continue;
        }
        const FieldDef& fd = def(f.attr);
        if (fd.level > level) {
            ++st.above_level;
            continue;
        }
        if (!type_matches(fd.type, f.value)) {
            ++st.wrong_type;
            continue;
        }
        // A later value for the same attribute replaces the earlier one.
        std::erase_if(r.attrs, [&](const Field& x) { return x.attr == f.attr; });
        if (fd.type == Type::domain) {
            std::string d = domain_of(std::get<std::string>(f.value));
            if (d.empty()) continue;
            r.attrs.emplace_back(f.attr, std::move(d));
        } else if (fd.type == Type::number && std::holds_alternative<long long>(f.value)) {
            r.attrs.emplace_back(f.attr, static_cast<double>(std::get<long long>(f.value)));
        } else if (const auto* s = std::get_if<std::string>(&f.value)) {
            const size_t max = fd.level == Level::content ? kMaxContentString : kMaxMetadataString;
            // Cut before redaction too, so a pathological input cannot make redaction slow.
            r.attrs.emplace_back(f.attr, cut(redactor.apply(cut(*s, max * 2)), max));
        } else {
            r.attrs.push_back(f);
        }
    }
    return r;
}

json to_json(const Record& r) {
    json attrs = json::object();
    for (const Field& f : r.attrs) {
        const char* name = def(f.attr).name;
        std::visit([&](const auto& v) { attrs[name] = v; }, f.value);
    }
    return {{"event", def(r.event).name}, {"time", r.time_ms}, {"attrs", std::move(attrs)}};
}

std::optional<Attr> attr_by_name(std::string_view name) {
    for (size_t i = 0; i < std::size(kAttrs); ++i)
        if (name == kAttrs[i].name) return static_cast<Attr>(i);
    return std::nullopt;
}

std::optional<Event> event_by_name(std::string_view name) {
    for (size_t i = 0; i < std::size(kEvents); ++i)
        if (name == kEvents[i].name) return static_cast<Event>(i);
    return std::nullopt;
}

std::optional<Record> record_from_json(const json& j) {
    if (!j.is_object()) return std::nullopt;
    const auto e = event_by_name(jstr(j, "event"));
    if (!e) return std::nullopt;
    Record r;
    r.event = *e;
    r.time_ms = jint(j, "time");
    if (j.contains("attrs") && j["attrs"].is_object())
        for (const auto& [k, v] : j["attrs"].items()) {
            const auto a = attr_by_name(k);
            if (!a) continue;
            if (v.is_string()) r.attrs.emplace_back(*a, v.get<std::string>());
            else if (v.is_boolean()) r.attrs.emplace_back(*a, v.get<bool>());
            else if (v.is_number_integer()) r.attrs.emplace_back(*a, v.get<long long>());
            else if (v.is_number()) r.attrs.emplace_back(*a, v.get<double>());
        }
    return r;
}

}  // namespace wb::telemetry
