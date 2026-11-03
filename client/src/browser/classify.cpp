#include "browser/classify.h"

#include "base/strings.h"
#include "base/url.h"

#include <array>
#include <cctype>

namespace wb::browser {

namespace {

// Word stems of submit and confirm controls (design D4), folded.
constexpr std::string_view kSubmitStems[] = {
    // en
    "submit", "send", "confirm", "order", "pay", "save", "delete", "buy", "purchase", "checkout", "place order",
    // cs: odeslat, potvrdit/potvrzení, objednat, zaplatit, uložit, smazat, koupit, schválit
    "odesl", "potvrd", "potvrz", "objedn", "zaplat", "uloz", "smaz", "koup", "schval",
    // de: senden, absenden, bestätigen, bestellen, bezahlen, zahlungspflichtig, speichern, löschen, kaufen, übermitteln
    "senden", "absend", "bestatig", "bestell", "bezahl", "zahlungspflichtig", "speicher", "losch", "kauf", "ubermittel",
    "einreich", "genehmig",
};

bool word_starts_with_stem(const std::string& name) {
    for (const std::string_view stem : kSubmitStems) {
        for (std::size_t pos = name.find(stem); pos != std::string::npos; pos = name.find(stem, pos + 1)) {
            const bool word_start = pos == 0 || !std::isalnum(static_cast<unsigned char>(name[pos - 1]));
            if (word_start) return true;
        }
    }
    return false;
}

bool is_ip_literal(std::string_view host) {
    if (host.find(':') != std::string_view::npos) return true;  // IPv6
    for (const char c : host)
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.')) return false;
    return !host.empty();
}

}  // namespace

bool is_submitting(const ElementFacts& e) {
    const std::string tag = to_lower(e.tag), type = to_lower(e.type), role = to_lower(e.role);
    if (tag == "input" && (type == "submit" || type == "image")) return true;
    if (tag == "button" && (type.empty() || type == "submit") && e.in_form) return true;  // a button submits by default
    if (tag == "button" && e.in_form && e.form_has_action) return true;
    const bool control = tag == "button" || (tag == "input" && (type == "button" || type == "reset")) || role == "button" ||
                         role == "menuitem";
    return control && word_starts_with_stem(fold_latin(e.name));
}

bool is_credential_field(const ElementFacts& e) {
    if (to_lower(e.tag) == "input" && to_lower(e.type) == "password") return true;
    for (const auto& token : split(to_lower(e.autocomplete), ' '))
        if (token == "current-password" || token == "new-password" || token == "one-time-code") return true;
    return false;
}

std::string registrable_domain(std::string_view host_in) {
    std::string host = to_lower(host_in);
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty() || is_ip_literal(host)) return host;
    const auto labels = split(host, '.');
    if (labels.size() <= 2) return host;
    const std::string& tld = labels[labels.size() - 1];
    const std::string& second = labels[labels.size() - 2];
    static const std::array<std::string_view, 6> kSecondLevel = {"co", "com", "ac", "gov", "org", "net"};
    bool public_suffix = false;
    if (tld.size() == 2)
        for (const auto s : kSecondLevel) public_suffix |= second == s;
    const std::size_t keep = public_suffix ? 3 : 2;
    std::string out;
    for (std::size_t i = labels.size() - keep; i < labels.size(); ++i) {
        if (!out.empty()) out.push_back('.');
        out += labels[i];
    }
    return out;
}

std::string url_host(std::string_view url) {
    auto u = parse_url(url);
    if (!u || (u->scheme != "http" && u->scheme != "https")) return {};
    return u->host;
}

bool same_site(std::string_view a, std::string_view b) {
    const std::string ha = url_host(a), hb = url_host(b);
    if (ha.empty() || hb.empty()) return false;
    return registrable_domain(ha) == registrable_domain(hb);
}

bool is_sign_in_host(std::string_view host_in, const std::vector<std::string>& idp_hosts) {
    const std::string host = to_lower(host_in);
    if (host.empty()) return false;
    std::vector<std::string> all = idp_hosts;
    for (const char* h : {"login.microsoftonline.com", "login.live.com", "login.windows.net", "accounts.google.com"}) all.emplace_back(h);
    for (const auto& raw : all) {
        const std::string h = to_lower(raw);
        if (h.empty()) continue;
        if (host == h || (host.size() > h.size() && ends_with(host, "." + h))) return true;
    }
    return false;
}

}  // namespace wb::browser
