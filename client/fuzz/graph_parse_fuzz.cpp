// Fuzz target for the Graph JSON response parsers (client/mail-calendar; add-mail-calendar task 3.3): message and event
// parsing, label extraction, exclusion rules and the tool-facing JSON must give an answer for any response body, never
// a crash, and a withheld item must never carry content.
#include "base/time.h"
#include "fuzz_main.h"
#include "mail/graph_parse.h"

using namespace wb;
using namespace wb::mail;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    (void)parse_iso8601_ms(text);
    (void)parse_msip_labels(text);

    const auto j = parse_json(text);
    if (!j) return 0;
    Rules rules;
    rules.excluded_labels = {"0f1e7b2a-1c39-4a0d-9d41-7a3f00000004"};
    rules.exclude_unlabelled = size % 2 == 0;
    rules.folders = {"junkemail"};
    rules.categories = {"private"};
    std::vector<json> items;
    if (j->is_object() && j->contains("value") && (*j)["value"].is_array())
        for (const auto& x : (*j)["value"]) items.push_back(x);
    else
        items.push_back(*j);
    for (const auto& item : items) {
        (void)item_label(item);
        Disposition d = Disposition::include;
        if (auto m = parse_message(item, rules, {"inbox", "Inbox"}, &d)) {
            const json shown = message_json(*m, true);
            if (m->withheld && (shown.contains("subject") || shown.contains("body"))) __builtin_trap();
            if (d != Disposition::include && !m->withheld) __builtin_trap();
        }
        if (auto e = parse_event(item, rules, &d)) {
            const json shown = event_json(*e, true);
            if ((e->withheld || e->masked) && (shown.contains("subject") || shown.contains("attendees"))) __builtin_trap();
        }
        if (item.is_object())
            for (const char* k : {"start", "end", "receivedDateTime"})
                if (item.contains(k)) (void)graph_time(item[k]);
    }
    return 0;
}
