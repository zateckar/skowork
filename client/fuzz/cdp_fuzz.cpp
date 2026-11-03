// Fuzz target for the agent browser's CDP input (client/browser-use; add-browser-use design D1, D2): everything Chrome
// sends comes from pages the agent visits. Framing, message parsing, the snapshot and the classifiers must give an
// answer for any input, never a crash or unbounded output.
#include "browser/cdp_frame.h"
#include "browser/classify.h"
#include "browser/snapshot.h"
#include "fuzz_main.h"

using namespace wb;
using namespace wb::browser;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    // The pipe stream in two arbitrary chunks; no message is longer than the limit or contains the terminator.
    CdpFramer framer(4096);
    std::vector<std::string> messages;
    const size_t cut = size ? data[0] % (size + 1) : 0;
    if (framer.feed(text.substr(0, cut), messages)) (void)framer.feed(text.substr(cut), messages);
    for (const auto& m : messages)
        if (m.size() > 4096 || m.find('\0') != std::string::npos) __builtin_trap();

    const CdpMessage m = parse_cdp_message(text);
    if (m.kind == CdpMessage::Kind::event && m.method.empty()) __builtin_trap();
    if (m.kind == CdpMessage::Kind::event && !m.params.is_object()) __builtin_trap();

    // The snapshot over whatever the input parses to, as a node array or the params of a reply.
    RefTable refs;
    const auto j = parse_json(text);
    if (j) {
        SnapshotOptions o;
        o.max_chars = 4000;
        const Snapshot s = build_snapshot(j->is_object() && j->contains("nodes") ? (*j)["nodes"] : *j, refs, o);
        if (s.text.size() > o.max_chars + 200) __builtin_trap();
        if (refs.size() > s.interactive) __builtin_trap();  // a reference only for a listed element
    }

    // Classifiers over input split into fields.
    const size_t a = size / 3, b = 2 * size / 3;
    ElementFacts f;
    f.tag = std::string(text.substr(0, a));
    f.type = std::string(text.substr(a, b - a));
    f.name = std::string(text.substr(b));
    f.role = f.type;
    f.autocomplete = f.name;
    f.in_form = size % 2 == 0;
    (void)is_submitting(f);
    (void)is_credential_field(f);
    const std::string host = url_host(text);
    if (host.find('/') != std::string::npos) __builtin_trap();
    if (registrable_domain(text).size() > size) __builtin_trap();
    (void)is_sign_in_host(text, {std::string(text.substr(0, a))});
    return 0;
}
