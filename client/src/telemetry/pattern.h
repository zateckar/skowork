// The matcher for policy redaction patterns (add-telemetry-and-admin design D8; client/telemetry Secret Redaction).
// ECMAScript syntax without back-references and lookaround, compiled to a program of at most kMaxProgram
// instructions and run as a Pike VM: one pass over the text per search, so time is linear in the text times the
// program. Byte-oriented, like std::regex over std::string.
#pragma once

#include <bitset>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::telemetry {

class Pattern {
public:
    static constexpr size_t kMaxProgram = 2000;

    // The compiled pattern, or nullopt with the reason in `error`.
    static std::optional<Pattern> compile(std::string_view pattern, std::string* error = nullptr);

    // The leftmost-first non-empty match starting at or after `from` ([begin, end)). Each step of the VM costs one
    // unit of `budget`; false with budget 0 when it runs out.
    bool search(std::string_view text, size_t from, size_t& begin, size_t& end, long long& budget) const;

    // Replaces each non-empty match, left to right. Returns nullopt when `budget` runs out.
    std::optional<std::string> replace(std::string_view text, std::string_view with, long long& budget) const;

    // True if the pattern can match the empty string (such matches are never replaced).
    [[nodiscard]] bool nullable() const { return nullable_; }

    struct Inst {
        enum class Op : std::uint8_t { byte, any, set, split, jmp, assert_bol, assert_eol, assert_word, assert_not_word, match };
        Op op;
        std::uint8_t c = 0;
        std::uint32_t x = 0, y = 0;  // split: x preferred; jmp: x; set: x is the index into sets_
    };

private:
    std::vector<Inst> prog_;
    std::vector<std::bitset<256>> sets_;
    bool nullable_ = false;
};

}  // namespace wb::telemetry
