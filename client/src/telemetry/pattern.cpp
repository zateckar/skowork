#include "telemetry/pattern.h"

#include <memory>

namespace wb::telemetry {
namespace {

using Set = std::bitset<256>;

bool is_word(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

Set class_set(char k) {
    Set s;
    for (int c = 0; c < 256; ++c) {
        const auto u = static_cast<unsigned char>(c);
        switch (k) {
        case 'd': case 'D': s[c] = u >= '0' && u <= '9'; break;
        case 'w': case 'W': s[c] = is_word(u); break;
        case 's': case 'S': s[c] = u == ' ' || (u >= '\t' && u <= '\r'); break;
        default: break;
        }
    }
    if (k == 'D' || k == 'W' || k == 'S') s.flip();
    return s;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool is_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

struct Node {
    enum class K { empty, byte, any, set, cat, alt, rep, bol, eol, word, not_word } k = K::empty;
    unsigned char c = 0;
    Set set;
    std::vector<Node> kids;
    int min = 0, max = 0;  // rep; max -1 is unbounded
    bool greedy = true;
};

constexpr int kMaxRepeat = 1000;
constexpr int kMaxDepth = 200;

class Parser {
public:
    explicit Parser(std::string_view p) : p_(p) {}

    std::optional<Node> parse(std::string& error) {
        Node n;
        if (!alt(n, 0)) {
            error = error_;
            return std::nullopt;
        }
        if (i_ != p_.size()) {
            error = "unmatched ')'";
            return std::nullopt;
        }
        return n;
    }

private:
    bool fail(const char* why) {
        if (error_.empty()) error_ = why;
        return false;
    }
    [[nodiscard]] bool at_end() const { return i_ >= p_.size(); }
    [[nodiscard]] char peek(size_t ahead = 0) const { return i_ + ahead < p_.size() ? p_[i_ + ahead] : '\0'; }

    bool alt(Node& out, int depth) {
        if (depth > kMaxDepth) return fail("nested too deeply");
        Node first;
        if (!cat(first, depth)) return false;
        if (at_end() || peek() != '|') {
            out = std::move(first);
            return true;
        }
        out.k = Node::K::alt;
        out.kids.push_back(std::move(first));
        while (!at_end() && peek() == '|') {
            ++i_;
            Node next;
            if (!cat(next, depth)) return false;
            out.kids.push_back(std::move(next));
        }
        return true;
    }

    bool cat(Node& out, int depth) {
        out.k = Node::K::cat;
        while (!at_end() && peek() != '|' && peek() != ')') {
            Node t;
            if (!term(t, depth)) return false;
            out.kids.push_back(std::move(t));
        }
        return true;
    }

    // {n}, {n,}, {n,m} at i_ (on '{'); false if it is not a well-formed quantifier.
    bool counted(int& min, int& max) {
        size_t j = i_ + 1;
        auto number = [&](int& v) {
            const size_t s = j;
            long long n = 0;
            while (j < p_.size() && p_[j] >= '0' && p_[j] <= '9') {
                n = n * 10 + (p_[j] - '0');
                if (n > 1'000'000) n = 1'000'000;
                ++j;
            }
            v = static_cast<int>(n);
            return j > s;
        };
        if (!number(min)) return false;
        max = min;
        if (j < p_.size() && p_[j] == ',') {
            ++j;
            if (j < p_.size() && p_[j] == '}') max = -1;
            else if (!number(max)) return false;
        }
        if (j >= p_.size() || p_[j] != '}') return false;
        i_ = j + 1;
        return true;
    }

    bool quantifier(int& min, int& max) {
        switch (peek()) {
        case '*': ++i_; min = 0; max = -1; return true;
        case '+': ++i_; min = 1; max = -1; return true;
        case '?': ++i_; min = 0; max = 1; return true;
        case '{':
            if (!counted(min, max)) return fail("'{' is not a quantifier");
            if (min > kMaxRepeat || max > kMaxRepeat || (max >= 0 && max < min)) return fail("bad repeat count");
            return true;
        default: return false;
        }
    }

    bool term(Node& out, int depth) {
        if (!atom(out, depth)) return false;
        int min = 0, max = 0;
        if (!quantifier(min, max)) return error_.empty();
        if (out.k == Node::K::bol || out.k == Node::K::eol || out.k == Node::K::word || out.k == Node::K::not_word)
            return fail("nothing to repeat");
        bool greedy = true;
        if (peek() == '?') {
            ++i_;
            greedy = false;
        }
        const char next = peek();
        if (next == '*' || next == '+' || next == '?' || next == '{') return fail("a quantifier follows a quantifier");
        Node r;
        r.k = Node::K::rep;
        r.min = min;
        r.max = max;
        r.greedy = greedy;
        r.kids.push_back(std::move(out));
        out = std::move(r);
        return true;
    }

    bool escape_byte(char e, unsigned char& c) {
        switch (e) {
        case 'n': c = '\n'; return true;
        case 'r': c = '\r'; return true;
        case 't': c = '\t'; return true;
        case 'f': c = '\f'; return true;
        case 'v': c = '\v'; return true;
        case '0':
            if (peek() >= '0' && peek() <= '9') return fail("back-references are not supported");
            c = 0;
            return true;
        case 'x': {
            const int h = hex(peek()), l = hex(peek(1));
            if (h < 0 || l < 0) return fail("bad \\x escape");
            i_ += 2;
            c = static_cast<unsigned char>(h * 16 + l);
            return true;
        }
        default:
            if (e >= '1' && e <= '9') return fail("back-references are not supported");
            if (is_alnum(e)) return fail("unknown escape");
            c = static_cast<unsigned char>(e);
            return true;
        }
    }

    bool atom(Node& out, int depth) {
        const char ch = peek();
        ++i_;
        switch (ch) {
        case '^': out.k = Node::K::bol; return true;
        case '$': out.k = Node::K::eol; return true;
        case '.': out.k = Node::K::any; return true;
        case '*': case '+': case '?': case '{': return fail("nothing to repeat");
        case '(': {
            if (peek() == '?') {
                if (peek(1) != ':') return fail("lookaround and named groups are not supported");
                i_ += 2;
            }
            if (!alt(out, depth + 1)) return false;
            if (peek() != ')') return fail("missing ')'");
            ++i_;
            return true;
        }
        case '[': return bracket(out);
        case '\\': {
            if (at_end()) return fail("trailing '\\'");
            const char e = p_[i_++];
            switch (e) {
            case 'b': out.k = Node::K::word; return true;
            case 'B': out.k = Node::K::not_word; return true;
            case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
                out.k = Node::K::set;
                out.set = class_set(e);
                return true;
            default: break;
            }
            out.k = Node::K::byte;
            return escape_byte(e, out.c);
        }
        default:
            out.k = Node::K::byte;
            out.c = static_cast<unsigned char>(ch);
            return true;
        }
    }

    // One bracket item: a byte (true, in c) or a class escape (false, merged into s).
    bool class_atom(bool& single, unsigned char& c, Set& s) {
        const char ch = p_[i_++];
        single = true;
        if (ch == '[' && (peek() == ':' || peek() == '.' || peek() == '=')) return fail("POSIX classes are not supported");
        if (ch != '\\') {
            c = static_cast<unsigned char>(ch);
            return true;
        }
        if (at_end()) return fail("trailing '\\'");
        const char e = p_[i_++];
        switch (e) {
        case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
            single = false;
            s |= class_set(e);
            return true;
        case 'b': c = 0x08; return true;
        case '-': c = '-'; return true;
        default: return escape_byte(e, c);
        }
    }

    bool bracket(Node& out) {
        out.k = Node::K::set;
        bool negate = false;
        if (peek() == '^') {
            negate = true;
            ++i_;
        }
        if (peek() == ']') return fail("empty class");
        Set s;
        while (true) {
            if (at_end()) return fail("missing ']'");
            if (peek() == ']') {
                ++i_;
                break;
            }
            bool single = false;
            unsigned char lo = 0;
            if (!class_atom(single, lo, s)) return false;
            // A range needs a bound after '-': not ']' and not the end of the pattern (a NUL byte is a bound, not the end).
            if (peek() == '-' && i_ + 1 < p_.size() && peek(1) != ']') {
                ++i_;
                bool single_hi = false;
                unsigned char hi = 0;
                if (!class_atom(single_hi, hi, s)) return false;
                if (!single || !single_hi) return fail("a class escape cannot bound a range");
                if (lo > hi) return fail("range out of order");
                for (int c = lo; c <= hi; ++c) s[c] = true;
                continue;
            }
            if (single) s[lo] = true;
        }
        if (negate) s.flip();
        out.set = s;
        return true;
    }

    std::string_view p_;
    size_t i_ = 0;
    std::string error_;
};

bool can_be_empty(const Node& n) {
    switch (n.k) {
    case Node::K::byte:
    case Node::K::any:
    case Node::K::set: return false;
    case Node::K::cat:
        for (const auto& k : n.kids)
            if (!can_be_empty(k)) return false;
        return true;
    case Node::K::alt:
        for (const auto& k : n.kids)
            if (can_be_empty(k)) return true;
        return false;
    case Node::K::rep: return n.min == 0 || can_be_empty(n.kids[0]);
    default: return true;
    }
}

}  // namespace

class PatternCompiler {
public:
    using Inst = Pattern::Inst;
    std::vector<Inst> prog;
    std::vector<std::bitset<256>> sets;

    bool emit(const Node& n) {
        if (prog.size() > Pattern::kMaxProgram) return false;
        switch (n.k) {
        case Node::K::empty: return true;
        case Node::K::byte: return push({Inst::Op::byte, n.c});
        case Node::K::any: return push({Inst::Op::any});
        case Node::K::set:
            sets.push_back(n.set);
            return push({Inst::Op::set, 0, static_cast<std::uint32_t>(sets.size() - 1)});
        case Node::K::bol: return push({Inst::Op::assert_bol});
        case Node::K::eol: return push({Inst::Op::assert_eol});
        case Node::K::word: return push({Inst::Op::assert_word});
        case Node::K::not_word: return push({Inst::Op::assert_not_word});
        case Node::K::cat:
            for (const auto& k : n.kids)
                if (!emit(k)) return false;
            return true;
        case Node::K::alt: {
            std::vector<size_t> jumps;
            for (size_t i = 0; i + 1 < n.kids.size(); ++i) {
                const size_t split = prog.size();
                if (!push({Inst::Op::split})) return false;
                prog[split].x = here();
                if (!emit(n.kids[i])) return false;
                jumps.push_back(prog.size());
                if (!push({Inst::Op::jmp})) return false;
                prog[split].y = here();
            }
            if (!emit(n.kids.back())) return false;
            for (const size_t j : jumps) prog[j].x = here();
            return true;
        }
        case Node::K::rep: {
            const Node& body = n.kids[0];
            for (int i = 0; i < n.min; ++i)
                if (!emit(body)) return false;
            if (n.max < 0) {
                const size_t split = prog.size();
                if (!push({Inst::Op::split})) return false;
                const std::uint32_t enter = here();
                if (!emit(body)) return false;
                if (!push({Inst::Op::jmp, 0, static_cast<std::uint32_t>(split)})) return false;
                set_split(split, enter, here(), n.greedy);
                return true;
            }
            std::vector<size_t> splits;
            for (int i = n.min; i < n.max; ++i) {
                splits.push_back(prog.size());
                if (!push({Inst::Op::split})) return false;
                prog[splits.back()].x = here();
                if (!emit(body)) return false;
            }
            for (const size_t s : splits) set_split(s, prog[s].x, here(), n.greedy);
            return true;
        }
        }
        return false;
    }
    bool push_match() { return push({Inst::Op::match}); }

private:
    bool push(Inst i) {
        prog.push_back(i);
        return prog.size() <= Pattern::kMaxProgram;
    }
    [[nodiscard]] std::uint32_t here() const { return static_cast<std::uint32_t>(prog.size()); }
    void set_split(size_t at, std::uint32_t enter, std::uint32_t skip, bool greedy) {
        prog[at].x = greedy ? enter : skip;
        prog[at].y = greedy ? skip : enter;
    }
};

std::optional<Pattern> Pattern::compile(std::string_view pattern, std::string* error) {
    std::string why;
    Parser parser(pattern);
    auto root = parser.parse(why);
    if (!root) {
        if (error) *error = why;
        return std::nullopt;
    }
    PatternCompiler pc;
    if (!pc.emit(*root) || !pc.push_match()) {
        if (error) *error = "pattern too large";
        return std::nullopt;
    }
    Pattern p;
    p.prog_ = std::move(pc.prog);
    p.sets_ = std::move(pc.sets);
    p.nullable_ = can_be_empty(*root);
    return p;
}

bool Pattern::search(std::string_view text, size_t from, size_t& begin, size_t& end, long long& budget) const {
    // Pike VM: threads in priority order (earlier start first, then the preferred branch), each program position at
    // most once per text position, so a search is one pass over the text.
    struct Thread {
        std::uint32_t pc;
        size_t start;
    };
    const size_t n = text.size();
    std::vector<Thread> clist, nlist, stack;
    std::vector<size_t> mark(prog_.size(), 0);  // generation (text position + 1) in which a pc was added
    auto word_at = [&](size_t i) { return i < n && is_word(static_cast<unsigned char>(text[i])); };

    auto add = [&](std::vector<Thread>& list, std::uint32_t pc, size_t start, size_t pos) {
        const size_t gen = pos + 1;
        stack.push_back({pc, start});
        while (!stack.empty()) {
            const Thread t = stack.back();
            stack.pop_back();
            if (mark[t.pc] == gen) continue;
            mark[t.pc] = gen;
            --budget;
            const Inst& in = prog_[t.pc];
            switch (in.op) {
            case Inst::Op::jmp: stack.push_back({in.x, t.start}); break;
            case Inst::Op::split:
                stack.push_back({in.y, t.start});
                stack.push_back({in.x, t.start});
                break;
            case Inst::Op::assert_bol:
                if (pos == 0) stack.push_back({t.pc + 1, t.start});
                break;
            case Inst::Op::assert_eol:
                if (pos == n) stack.push_back({t.pc + 1, t.start});
                break;
            case Inst::Op::assert_word:
            case Inst::Op::assert_not_word: {
                const bool boundary = (pos > 0 && word_at(pos - 1)) != word_at(pos);
                if (boundary == (in.op == Inst::Op::assert_word)) stack.push_back({t.pc + 1, t.start});
                break;
            }
            default: list.push_back(t); break;
            }
        }
    };

    bool matched = false;
    for (size_t pos = from; pos <= n; ++pos) {
        if (budget <= 0) return false;
        if (!matched) add(clist, 0, pos, pos);
        if (clist.empty()) {
            if (matched) break;
            continue;
        }
        nlist.clear();
        for (const Thread& t : clist) {
            --budget;
            const Inst& in = prog_[t.pc];
            if (in.op == Inst::Op::match) {
                if (t.start == pos) continue;  // empty matches are not reported
                matched = true;
                begin = t.start;
                end = pos;
                break;  // lower-priority threads lose
            }
            if (pos >= n) continue;
            const auto c = static_cast<unsigned char>(text[pos]);
            const bool ok = in.op == Inst::Op::byte ? c == in.c : in.op == Inst::Op::any ? (c != '\n' && c != '\r') : sets_[in.x][c];
            if (ok) add(nlist, t.pc + 1, t.start, pos + 1);
        }
        std::swap(clist, nlist);
    }
    return matched && budget > 0;
}

std::optional<std::string> Pattern::replace(std::string_view text, std::string_view with, long long& budget) const {
    std::string out;
    size_t pos = 0;
    size_t b = 0, e = 0;
    while (pos <= text.size() && search(text, pos, b, e, budget)) {
        out.append(text.substr(pos, b - pos));
        out.append(with);
        pos = e;
    }
    if (budget <= 0) return std::nullopt;
    out.append(text.substr(pos));
    return out;
}

}  // namespace wb::telemetry
