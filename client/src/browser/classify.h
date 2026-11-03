// Classification of browser actions (client/browser-use Declared Risk Classes, Submitting Actions Are External and No
// Password Entry By Agent; add-browser-use design D3, D4, D8). Pure functions over facts read from the page.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace wb::browser {

// What the client reads about an element before acting on it.
struct ElementFacts {
    std::string tag;           // lower-case tag name: button, input, a, select, div ...
    std::string type;          // lower-case type attribute (input, button)
    std::string role;          // accessibility role (button, link, textbox ...)
    std::string name;          // accessible name or visible text
    std::string autocomplete;  // lower-case autocomplete attribute
    bool in_form = false;      // inside a <form> (or associated with one)
    bool form_has_action = false;
};

// A click on this element submits or confirms something: a submit button or input, a button inside a form with an
// action, or a control whose name is a submit/confirm word in cs, de or en.
bool is_submitting(const ElementFacts& e);

// Typing into this element would enter a credential: a password input, or a current-password, new-password or
// one-time-code autocomplete field.
bool is_credential_field(const ElementFacts& e);

// The registrable domain of a host: the last two labels, or three when the second-level label is a common public
// suffix under a two-letter country code (co.uk, com.au ...). IP addresses and single-label hosts are returned as is.
std::string registrable_domain(std::string_view host);

// True when both URLs are on the same registrable domain (and neither is empty).
bool same_site(std::string_view url_a, std::string_view url_b);

// The host of an http(s) URL, lower-case; empty for other schemes.
std::string url_host(std::string_view url);

// True for hosts of identity-provider sign-in pages: `idp_hosts` (exact or a parent domain), plus the well-known
// Microsoft and Google sign-in hosts.
bool is_sign_in_host(std::string_view host, const std::vector<std::string>& idp_hosts);

}  // namespace wb::browser
