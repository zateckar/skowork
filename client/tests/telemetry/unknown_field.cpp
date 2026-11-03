// add-telemetry-and-admin task 2.1: telemetry code names fields only through the generated catalogue, so a field that
// telemetry/catalogue.yaml does not list does not compile. The build of this file with WORKBENCH_UNKNOWN_FIELD is a
// ctest expected to fail; without it, the same file must compile (so the failure is the unknown field, nothing else).
#include "telemetry/record.h"

namespace wb::telemetry {

std::optional<Record> catalogue_probe(const Redactor& r) {
#ifdef WORKBENCH_UNKNOWN_FIELD
    return make_record(Event::tool_call, 0, {{Attr::user_email, "someone@example.com"}}, Level::metadata, r);
#else
    return make_record(Event::tool_call, 0, {{Attr::tool_id, "files.read"}}, Level::metadata, r);
#endif
}

}  // namespace wb::telemetry
