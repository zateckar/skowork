// client/telemetry portable core (add-telemetry-and-admin tasks 2.1, 3.1, 3.2): the generated allowlist, level
// filtering, secret redaction with policy patterns, URL-to-domain reduction and the OTLP log encoding.
#include "telemetry/otlp.h"
#include "telemetry/record.h"

#include <gtest/gtest.h>

#include <chrono>
#include <set>

using namespace wb;
using namespace wb::telemetry;

namespace {

const Value* find(const Record& r, Attr a) {
    for (const auto& f : r.attrs)
        if (f.attr == a) return &f.value;
    return nullptr;
}
std::string str(const Record& r, Attr a) {
    const Value* v = find(r, a);
    return v && std::holds_alternative<std::string>(*v) ? std::get<std::string>(*v) : std::string("<none>");
}

}  // namespace

TEST(TelemetryCatalogue, NamesAreUniqueAndEveryEventListsKnownAttributes) {
    std::set<std::string> names;
    for (const auto& a : kAttrs) EXPECT_TRUE(names.insert(a.name).second) << a.name;
    for (const auto& r : kResource) EXPECT_TRUE(names.insert(r.name).second) << r.name;
    for (const auto& e : kEvents) {
        EXPECT_FALSE(e.attrs.empty()) << e.name;
        EXPECT_EQ(event_by_name(e.name).has_value(), true);
    }
    EXPECT_EQ(def(Attr::tool_id).level, Level::metadata);
    EXPECT_EQ(def(Attr::content_prompt).level, Level::content);
    EXPECT_EQ(def(Attr::url_domain).type, Type::domain);
}

TEST(TelemetryRecord, LevelOffProducesNothing) {
    const Redactor r;
    EXPECT_FALSE(make_record(Event::tool_call, 1, {{Attr::tool_id, "files.read"}}, Level::off, r));
    EXPECT_EQ(parse_level("off"), Level::off);
    EXPECT_EQ(parse_level("bogus"), std::nullopt);
}

TEST(TelemetryRecord, MetadataLevelDropsContentFields) {
    const Redactor r;
    FilterStats st;
    const auto rec = make_record(Event::tool_call, 1,
                                 {{Attr::tool_id, "files.read"}, {Attr::tool_risk, "read"}, {Attr::tool_outcome, "ok"},
                                  {Attr::tool_duration_ms, 12}, {Attr::tool_arguments, R"({"path":"C:\\secret\\plan.docx"})"}},
                                 Level::metadata, r, &st);
    ASSERT_TRUE(rec);
    EXPECT_EQ(str(*rec, Attr::tool_id), "files.read");
    EXPECT_EQ(find(*rec, Attr::tool_arguments), nullptr);
    EXPECT_EQ(st.above_level, 1);
    EXPECT_EQ(to_json(*rec).dump().find("plan.docx"), std::string::npos);

    const auto content = make_record(Event::tool_call, 1, {{Attr::tool_id, "files.read"}, {Attr::tool_arguments, "{}"}}, Level::content, r);
    ASSERT_TRUE(content);
    EXPECT_EQ(str(*content, Attr::tool_arguments), "{}");
}

TEST(TelemetryRecord, AttributesOutsideTheEventOrOfTheWrongTypeAreDropped) {
    const Redactor r;
    FilterStats st;
    const auto rec = make_record(Event::client_footprint, 1,
                                 {{Attr::process_working_set_mb, 41}, {Attr::tool_id, "x"}, {Attr::network_enforcement, 3}},
                                 Level::content, r, &st);
    ASSERT_TRUE(rec);
    ASSERT_EQ(rec->attrs.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(rec->attrs[0].value), 41.0);  // integers widen to number
    EXPECT_EQ(st.not_in_event, 2);
}

TEST(TelemetryRecord, UrlsAreReducedToTheDomain) {
    const Redactor r;
    const auto rec = make_record(Event::tool_call, 1, {{Attr::url_domain, "https://Wiki.Example.com:8443/team/plan?q=secret#x"}},
                                 Level::metadata, r);
    ASSERT_TRUE(rec);
    EXPECT_EQ(str(*rec, Attr::url_domain), "wiki.example.com");
    EXPECT_EQ(domain_of("user:pw@intranet.corp:80/x"), "intranet.corp");
    EXPECT_EQ(domain_of("[::1]:8080/a"), "::1");
    EXPECT_EQ(domain_of("not a host/with spaces"), "");
    const auto none = make_record(Event::tool_call, 1, {{Attr::url_domain, "%%%"}}, Level::metadata, r);
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->attrs.empty());
}

TEST(TelemetryRecord, TokensInErrorTextAreRedactedAtEveryLevel) {
    const Redactor r;
    const std::string jwt = "eyJhbGciOiJSUzI1NiJ9.eyJzdWIiOiJ1c2VyMSJ9.c2lnbmF0dXJlLXZhbHVlLWhlcmU";
    const auto rec = make_record(Event::client_error,
                                 1, {{Attr::error_code, "AUTH"}, {Attr::error_message, "call failed: Authorization: Bearer " + jwt}},
                                 Level::content, r);
    ASSERT_TRUE(rec);
    const std::string msg = str(*rec, Attr::error_message);
    EXPECT_EQ(msg.find(jwt), std::string::npos) << msg;
    EXPECT_NE(msg.find(kRedacted), std::string::npos) << msg;
    // A metadata string attribute goes through the same redaction.
    const auto meta = make_record(Event::client_error, 1, {{Attr::error_code, "refresh_token=" + std::string(48, 'a')}}, Level::metadata, r);
    ASSERT_TRUE(meta);
    EXPECT_EQ(str(*meta, Attr::error_code).find(std::string(48, 'a')), std::string::npos);
}

TEST(TelemetryRecord, PolicyRedactionPatternsAreAppliedAndBadPatternsSkipped) {
    const Redactor r({R"(\bPN-\d{6}\b)", "([unclosed"});
    ASSERT_EQ(r.invalid().size(), 1u);
    EXPECT_EQ(r.invalid()[0], "([unclosed");
    const auto rec = make_record(Event::client_error, 1, {{Attr::error_message, "no access for PN-123456 in HR"}}, Level::content, r);
    ASSERT_TRUE(rec);
    EXPECT_EQ(str(*rec, Attr::error_message), "no access for [redacted] in HR");
}

TEST(TelemetryRecord, PolicyPatternsCannotBacktrackExponentially) {
    // Found by fuzz_telemetry_redaction: with the backtracking executor this pattern took minutes on 110 characters.
    // Valid patterns that backtrack exponentially or polynomially with std::regex; back-references and stacked
    // quantifiers are refused.
    const Redactor r({"(a|b|ab)*(a|b)*c", "(.*)*(.*)*x", R"((\w+)-\1)", "(a|b)*******c"});
    ASSERT_EQ(r.invalid().size(), 2u);
    EXPECT_EQ(r.invalid()[0], R"((\w+)-\1)");
    // Runs of a and b shorter than the built-in 40-character token rule, so only the policy patterns see them.
    std::string text;
    for (int i = 0; i < 250; ++i) text += std::string(15, 'a') + std::string(15, 'b') + " ";
    const auto start = std::chrono::steady_clock::now();
    const auto rec = make_record(Event::client_error, 1, {{Attr::error_message, text}, {Attr::error_code, "abbac"}}, Level::content, r);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(rec);
    EXPECT_EQ(str(*rec, Attr::error_message), text);  // no match: unchanged
    EXPECT_EQ(str(*rec, Attr::error_code), "[redacted]");
    EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST(TelemetryPattern, EcmaScriptSubsetMatchesLeftmostFirst) {
    auto rep = [](std::string_view pattern, std::string_view text) {
        auto p = Pattern::compile(pattern);
        if (!p) return std::string("<invalid>");
        long long budget = kRedactionBudget;
        return p->replace(text, "#", budget).value_or("<budget>");
    };
    EXPECT_EQ(rep(R"(\bPN-\d{6}\b)", "PN-123456 xPN-123456 PN-1234567"), "# xPN-123456 PN-1234567");
    EXPECT_EQ(rep("a|ab", "abab"), "#b#b");  // leftmost-first, not longest
    EXPECT_EQ(rep("ab|a", "abab"), "##");
    EXPECT_EQ(rep("a+?b", "aaab"), "#");
    EXPECT_EQ(rep("<.+?>", "<a><b>"), "##");
    EXPECT_EQ(rep("<.+>", "<a><b>"), "#");
    EXPECT_EQ(rep("[^0-9a-f-]+", "12-ab:XY"), "12-ab#");
    EXPECT_EQ(rep(R"([\w.]+@[\w.]+)", "mail jan.novak@corp.cz now"), "mail # now");
    EXPECT_EQ(rep("(?:ab){2,3}", "ababababab"), "##");
    EXPECT_EQ(rep("^a", "aaa"), "#aa");
    EXPECT_EQ(rep("a$", "aaa"), "aa#");
    EXPECT_EQ(rep("x*", "abc"), "abc");  // empty matches are not replaced
    EXPECT_EQ(rep(R"(\x41\.)", "A.B"), "#B");
    EXPECT_EQ(rep("a.c", "a\nc abc"), "a\nc #");
    for (const char* bad : {R"((\w+)-\1)", "(?=a)", "(?!a)", "(?<n>a)", "a**", "a{3,2}", "*a", "[]", "[z-a]", "(a", "a)", R"(\q)",
                            "[[:alpha:]]", "a{1001}", "(((((a{1,1000}){1,1000})))))"})
        EXPECT_EQ(rep(bad, "a"), "<invalid>") << bad;
    // A NUL byte after '-' bounds a range; it is not the end of the pattern (fuzz finding).
    using namespace std::string_view_literals;
    EXPECT_EQ(rep("[\x01-\0]"sv, "a"), "<invalid>");
    EXPECT_EQ(rep("[\0-\x01]"sv, "a-\x01"sv), "a-#");
}

TEST(TelemetryPattern, AnOverBudgetTextIsRedactedWhole) {
    // Each search for ".*x" runs to the end of the text before falling back to "a": quadratic in the text, so a long
    // text runs out of budget and nothing of it is kept.
    const Redactor r({".*x|a"});
    std::string text;
    for (int i = 0; i < 800; ++i) text += "a b c d e f g h ";
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(r.apply(text), "[redacted]");
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
    EXPECT_EQ(r.apply("a b"), "[redacted] b");
}

TEST(TelemetryRecord, StringsAreCutPerLevelWithoutSplittingUtf8) {
    const Redactor r;
    std::string longer;
    for (int i = 0; i < 200; ++i) longer += "\xC5\xA1";  // š, 2 bytes each: 400 bytes
    const auto rec = make_record(Event::tool_call, 1, {{Attr::tool_id, longer}}, Level::metadata, r);
    ASSERT_TRUE(rec);
    const std::string s = str(*rec, Attr::tool_id);
    EXPECT_LE(s.size(), kMaxMetadataString);
    EXPECT_EQ(s.size() % 2, 0u);
}

TEST(TelemetryRecord, JsonRoundTrip) {
    const Redactor r;
    const auto rec = make_record(Event::policy_decision, 1700000000123,
                                 {{Attr::tool_id, "files.read"}, {Attr::tool_risk, "read"}, {Attr::policy_rule, "filesystem.deny[\"**\\\\.ssh\\\\**\"]"},
                                  {Attr::policy_version, 7}, {Attr::policy_outcome, "deny"}},
                                 Level::metadata, r);
    ASSERT_TRUE(rec);
    const json j = to_json(*rec);
    EXPECT_EQ(jstr(j, "event"), "policy.decision");
    EXPECT_EQ(jint(j["attrs"], "policy.version"), 7);
    const auto back = record_from_json(j);
    ASSERT_TRUE(back);
    EXPECT_EQ(to_json(*back), j);
    EXPECT_FALSE(record_from_json(json{{"event", "nope"}}));
}

TEST(TelemetryOtlp, EncodesLogRecordsThatDecodeBack) {
    const Redactor r;
    std::vector<Record> recs;
    recs.push_back(*make_record(Event::run_finished, 1700000000001, {{Attr::run_id, "r1"}, {Attr::run_outcome, "succeeded"}, {Attr::run_duration_ms, 4200}}, Level::metadata, r));
    recs.push_back(*make_record(Event::client_footprint, 1700000000002, {{Attr::process_working_set_mb, 42.5}}, Level::metadata, r));
    recs.push_back(*make_record(Event::sandbox_enforcement, 1700000000003, {{Attr::network_enforcement, "proxy-only"}}, Level::metadata, r));
    ResourceInfo res;
    res.attrs = {{Res::service_version, "0.1.0"}, {Res::skowork_installation_id, "inst-1"}};
    const std::string bytes = encode_logs(res, recs, "0.1.0");
    const auto b = decode_logs(bytes);
    ASSERT_TRUE(b);
    EXPECT_EQ(b->scope_name, "skowork.client");
    ASSERT_EQ(b->resource.size(), 2u);
    EXPECT_EQ(b->resource[1].first, "skowork.installation_id");
    EXPECT_EQ(std::get<std::string>(b->resource[1].second), "inst-1");
    ASSERT_EQ(b->logs.size(), 3u);
    EXPECT_EQ(b->logs[0].event_name, "run.finished");
    EXPECT_EQ(b->logs[0].time_unix_nano, 1700000000001ULL * 1000000ULL);
    ASSERT_EQ(b->logs[0].attrs.size(), 3u);
    EXPECT_EQ(std::get<long long>(b->logs[0].attrs[2].second), 4200);
    EXPECT_DOUBLE_EQ(std::get<double>(b->logs[1].attrs[0].second), 42.5);
    EXPECT_EQ(std::get<std::string>(b->logs[2].attrs[0].second), "proxy-only");
    // Truncated input is refused rather than misread.
    EXPECT_FALSE(decode_logs(std::string_view(bytes).substr(0, bytes.size() - 3)));
}
