// IANA time zones through the ICU that ships with Windows (icu.dll, loaded at run time; design D2, D9).
#pragma once

#include "base/result.h"
#include "scheduler/cron.h"

#include <memory>
#include <string>

namespace wb::scheduler {

// The zone with this IANA id ("Europe/Prague"); fails for an id ICU does not know or when icu.dll is missing.
Result<std::shared_ptr<TimeZone>> icu_zone(const std::string& iana_id);

// The IANA id of the computer's time zone ("UTC" when it cannot be determined).
std::string local_zone_id();

}  // namespace wb::scheduler
