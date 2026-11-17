#pragma once

#include "base/result.h"
#include "model/types.h"

namespace wb::triage {

struct Event {
    std::string id, revision, text;
};

struct Classification {
    std::string id, revision, outcome, reason;
};

Result<std::vector<Classification>> classify(model::IChatModel& model, const std::string& responsibilities,
                                              const std::vector<Event>& events,
                                              const std::shared_ptr<CancelToken>& cancel = {});

}  // namespace wb::triage
