// Portable parts of client/browser-use (add-browser-use): CDP framing, the page snapshot and action classification.
#include "browser/cdp_frame.h"
#include "browser/classify.h"
#include "browser/snapshot.h"

#include <gtest/gtest.h>

using namespace wb;
using namespace wb::browser;

// ---- CDP framing ----------------------------------------------------------------------------------------------------

TEST(CdpFrame, SplitsNulDelimitedMessagesAcrossChunks) {
    CdpFramer f;
    std::vector<std::string> out;
    ASSERT_TRUE(f.feed(std::string_view("{\"id\":1}\0{\"id\"", 14), out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0], "{\"id\":1}");
    ASSERT_TRUE(f.feed(std::string_view(":2}\0", 4), out));
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1], "{\"id\":2}");
    EXPECT_EQ(f.buffered(), 0u);
}

TEST(CdpFrame, RefusesAMessageOverTheLimit) {
    CdpFramer f(16);
    std::vector<std::string> out;
    EXPECT_FALSE(f.feed(std::string(32, 'x'), out));
    EXPECT_FALSE(f.feed(std::string_view("\0", 1), out));  // unusable afterwards
    EXPECT_TRUE(out.empty());
}

TEST(CdpFrame, ParsesResponsesEventsAndErrors) {
    auto r = parse_cdp_message(R"({"id":7,"result":{"frameId":"F"},"sessionId":"S1"})");
    EXPECT_EQ(r.kind, CdpMessage::Kind::response);
    EXPECT_EQ(r.id, 7);
    EXPECT_EQ(jstr(r.result, "frameId"), "F");
    EXPECT_EQ(r.session_id, "S1");
    EXPECT_TRUE(r.error.empty());

    auto e = parse_cdp_message(R"({"id":8,"error":{"code":-32000,"message":"No node with given id found"}})");
    EXPECT_EQ(e.kind, CdpMessage::Kind::response);
    EXPECT_EQ(e.error, "No node with given id found");

    auto ev = parse_cdp_message(R"({"method":"Fetch.requestPaused","params":{"requestId":"r1"},"sessionId":"S2"})");
    EXPECT_EQ(ev.kind, CdpMessage::Kind::event);
    EXPECT_EQ(ev.method, "Fetch.requestPaused");
    EXPECT_EQ(jstr(ev.params, "requestId"), "r1");
    EXPECT_EQ(ev.session_id, "S2");

    for (const char* bad : {"", "nope", "[]", "{}", R"({"id":"x"})", R"({"method":5})", R"({"method":""})", "{\"id\":1"})
        EXPECT_EQ(parse_cdp_message(bad).kind, CdpMessage::Kind::invalid) << bad;
}

TEST(CdpFrame, EncodesCommandsWithTheTerminator) {
    const std::string c = encode_cdp_command(3, "Page.navigate", {{"url", "https://a.example/"}}, "S");
    ASSERT_FALSE(c.empty());
    EXPECT_EQ(c.back(), '\0');
    const json j = json::parse(c.substr(0, c.size() - 1));
    EXPECT_EQ(jint(j, "id"), 3);
    EXPECT_EQ(jstr(j, "method"), "Page.navigate");
    EXPECT_EQ(jstr(j["params"], "url"), "https://a.example/");
    EXPECT_EQ(jstr(j, "sessionId"), "S");
    const std::string b = encode_cdp_command(4, "Browser.getVersion", json::object());
    EXPECT_FALSE(json::parse(b.substr(0, b.size() - 1)).contains("sessionId"));
}

// ---- Snapshot -------------------------------------------------------------------------------------------------------

namespace {

json ax(const std::string& id, const std::string& role, const std::string& name, std::vector<std::string> children,
        long long backend = 0, json props = json::array(), const std::string& value = {}) {
    json n = {{"nodeId", id}, {"ignored", false}, {"role", {{"type", "role"}, {"value", role}}},
              {"name", {{"type", "computedString"}, {"value", name}}}, {"childIds", children}, {"properties", props}};
    if (backend) n["backendDOMNodeId"] = backend;
    if (!value.empty()) n["value"] = {{"type", "string"}, {"value", value}};
    return n;
}

json form_page() {
    return json::array({
        ax("1", "RootWebArea", "Order", {"2", "3", "9"}),
        ax("2", "heading", "Place an order", {"21"}, 0, json::array({{{"name", "level"}, {"value", {{"type", "integer"}, {"value", 1}}}}})),
        ax("21", "StaticText", "Place an order", {}),
        ax("3", "form", "", {"4", "5", "6", "7"}),
        ax("4", "textbox", "Quantity", {}, 104, json::array({{{"name", "required"}, {"value", {{"type", "boolean"}, {"value", true}}}}}), "2"),
        ax("5", "checkbox", "Express", {}, 105, json::array({{{"name", "checked"}, {"value", {{"type", "tristate"}, {"value", "true"}}}}})),
        ax("6", "button", "Odeslat", {"61"}, 106),
        ax("61", "StaticText", "Odeslat", {}),
        ax("7", "generic", "", {"71"}),
        ax("71", "StaticText", "Prices include VAT.", {}),
        ax("9", "table", "", {"10"}),
        ax("10", "row", "", {"11", "12"}),
        ax("11", "cell", "", {"111"}),
        ax("111", "StaticText", "Total", {}),
        ax("12", "cell", "", {"121"}),
        ax("121", "StaticText", "42 EUR", {}),
    });
}

}  // namespace

TEST(BrowserSnapshot, ListsInteractiveElementsWithReferencesAndStates) {
    RefTable refs;
    const Snapshot s = build_snapshot(form_page(), refs);
    EXPECT_EQ(s.interactive, 3u);
    EXPECT_FALSE(s.truncated);
    EXPECT_NE(s.text.find("heading \"Place an order\" [level=1]"), std::string::npos) << s.text;
    EXPECT_NE(s.text.find("textbox \"Quantity\" [ref=e1] value=\"2\" [required]"), std::string::npos) << s.text;
    EXPECT_NE(s.text.find("checkbox \"Express\" [ref=e2] [checked]"), std::string::npos) << s.text;
    EXPECT_NE(s.text.find("button \"Odeslat\" [ref=e3]"), std::string::npos) << s.text;
    EXPECT_NE(s.text.find("text: \"Prices include VAT.\""), std::string::npos) << s.text;
    EXPECT_NE(s.text.find("row: \"Total | 42 EUR\""), std::string::npos) << s.text;
    // The heading's and the button's own text is not repeated.
    EXPECT_EQ(s.text.find("text: \"Place an order\""), std::string::npos) << s.text;
    EXPECT_EQ(s.text.find("text: \"Odeslat\""), std::string::npos) << s.text;
    EXPECT_EQ(refs.resolve("e3"), 106);
}

TEST(BrowserSnapshot, ReferencesAreStableWithinADocumentAndStaleAfterReset) {
    RefTable refs;
    (void)build_snapshot(form_page(), refs);
    const Snapshot again = build_snapshot(form_page(), refs);
    EXPECT_NE(again.text.find("button \"Odeslat\" [ref=e3]"), std::string::npos);
    refs.reset();
    EXPECT_FALSE(refs.resolve("e3"));
    const Snapshot next = build_snapshot(form_page(), refs);
    EXPECT_NE(next.text.find("textbox \"Quantity\" [ref=e4]"), std::string::npos) << next.text;  // never reuses e1-e3
}

TEST(BrowserSnapshot, TruncatesLongPagesWithANote) {
    json nodes = json::array();
    std::vector<std::string> kids;
    for (int i = 0; i < 500; ++i) {
        kids.push_back("b" + std::to_string(i));
        nodes.push_back(ax("b" + std::to_string(i), "link", "Link number " + std::to_string(i), {}, 1000 + i));
    }
    nodes.insert(nodes.begin(), ax("root", "RootWebArea", "", kids));
    RefTable refs;
    const Snapshot s = build_snapshot(nodes, refs, SnapshotOptions{2000, 160});
    EXPECT_TRUE(s.truncated);
    EXPECT_LE(s.text.size(), 2100u);
    EXPECT_NE(s.text.find("[snapshot truncated"), std::string::npos);
}

TEST(BrowserSnapshot, SurvivesMalformedTrees) {
    RefTable refs;
    // A cycle, a missing child, wrong types and a deep chain.
    json nodes = json::array({ax("1", "RootWebArea", "", {"2", "404"}), ax("2", "group", "g", {"1"}), json(5),
                              json{{"nodeId", 3}}, json{{"nodeId", "x"}, {"role", "button"}}});
    EXPECT_NO_THROW((void)build_snapshot(nodes, refs));
    json deep = json::array();
    for (int i = 0; i < 5000; ++i)
        deep.push_back(ax(std::to_string(i), "group", "g", {std::to_string(i + 1)}));
    EXPECT_NO_THROW((void)build_snapshot(deep, refs));
    EXPECT_EQ(build_snapshot(json("nope"), refs).text, "");
}

// ---- Classification -------------------------------------------------------------------------------------------------

TEST(BrowserClassify, SubmittingControls) {
    EXPECT_TRUE(is_submitting({"input", "submit", "", "", "", true, false}));
    EXPECT_TRUE(is_submitting({"input", "image", "", "", "", false, false}));
    EXPECT_TRUE(is_submitting({"button", "", "button", "Weiter", "", true, false}));  // default type submit in a form
    EXPECT_FALSE(is_submitting({"button", "", "button", "Weiter", "", false, false}));
    EXPECT_TRUE(is_submitting({"button", "button", "button", "x", "", true, true}));  // a form with an action
    // Submit and confirm words in en, cs and de, on any control.
    for (const char* name : {"Send", "Submit order", "Confirm", "Odeslat", "Potvrdit", "Schválit", "Zaplatit", "Absenden",
                             "Bestätigen", "Senden", "Kaufen", "Delete", "Smazat", "Löschen", "Genehmigen"})
        EXPECT_TRUE(is_submitting({"div", "", "button", name, "", false, false})) << name;
    for (const char* name : {"Next page", "Další", "Open", "Show details", "Zobrazit", "Anzeigen"})
        EXPECT_FALSE(is_submitting({"div", "", "button", name, "", false, false})) << name;
    EXPECT_FALSE(is_submitting({"a", "", "link", "Send", "", false, false}));  // a link navigates
}

TEST(BrowserClassify, CredentialFields) {
    EXPECT_TRUE(is_credential_field({"input", "password", "textbox", "", "", true, false}));
    EXPECT_TRUE(is_credential_field({"input", "text", "textbox", "", "one-time-code", true, false}));
    EXPECT_TRUE(is_credential_field({"input", "text", "textbox", "", "current-password", true, false}));
    EXPECT_TRUE(is_credential_field({"input", "text", "textbox", "", "new-password", true, false}));
    EXPECT_FALSE(is_credential_field({"input", "text", "textbox", "Password hint", "username", true, false}));
    EXPECT_FALSE(is_credential_field({"textarea", "", "textbox", "", "", false, false}));
}

TEST(BrowserClassify, RegistrableDomainsAndSites) {
    EXPECT_EQ(registrable_domain("portal.intra.example.com"), "example.com");
    EXPECT_EQ(registrable_domain("www.bbc.co.uk"), "bbc.co.uk");
    EXPECT_EQ(registrable_domain("shop.example.com.au"), "example.com.au");
    EXPECT_EQ(registrable_domain("Example.COM."), "example.com");
    EXPECT_EQ(registrable_domain("127.0.0.1"), "127.0.0.1");
    EXPECT_EQ(registrable_domain("intranet"), "intranet");
    EXPECT_TRUE(same_site("https://a.example.com/x", "http://b.example.com/y"));
    EXPECT_FALSE(same_site("https://a.example.com/", "https://example.org/"));
    EXPECT_FALSE(same_site("", "https://a.example/"));
    EXPECT_EQ(url_host("https://Portal.Example.com:8443/p?q"), "portal.example.com");
    EXPECT_EQ(url_host("file:///C:/x"), "");
    EXPECT_EQ(url_host("javascript:alert(1)"), "");
}

TEST(BrowserClassify, SignInHosts) {
    const std::vector<std::string> idp = {"idp.example.com"};
    EXPECT_TRUE(is_sign_in_host("idp.example.com", idp));
    EXPECT_TRUE(is_sign_in_host("eu.idp.example.com", idp));
    EXPECT_FALSE(is_sign_in_host("notidp.example.com", idp));
    EXPECT_TRUE(is_sign_in_host("login.microsoftonline.com", {}));
    EXPECT_TRUE(is_sign_in_host("accounts.google.com", {}));
    EXPECT_FALSE(is_sign_in_host("portal.example.com", idp));
}
