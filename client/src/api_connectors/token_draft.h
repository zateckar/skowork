// The text of a personal access token being typed (client/api-connectors Personal Token Entry). It is held only while the
// field shows it: take() returns the text and clears it, so the field is empty once the token has been sent.
#pragma once

#include <string>
#include <utility>

namespace wb::connectors {

class TokenDraft {
public:
    void set(std::u16string text) { text_ = std::move(text); }
    [[nodiscard]] const std::u16string& text() const { return text_; }
    void clear() { text_.clear(); }
    [[nodiscard]] std::u16string take() {
        std::u16string out = std::move(text_);
        text_.clear();
        return out;
    }

private:
    std::u16string text_;
};

}  // namespace wb::connectors
