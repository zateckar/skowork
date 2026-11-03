// The Meetings settings section's host methods (client/meeting-capture Meetings Settings Section), UI only:
// meetings.status, .sources, .stop, .capture, .transcribe_now, .rename_speaker, .remove_series and .forget.
#pragma once

#include "host/rpc.h"
#include "meeting/dictation.h"
#include "meeting/meeting_service.h"

#include <functional>

namespace wb {

// `service`: the running meeting service, or null while it is not running.
void register_meeting_rpc(RpcRegistry& rpc, std::function<meeting::MeetingService*()> service);

// client/dictation (add-dictation D1): dictation.start, .stop and .cancel, UI only.
void register_dictation_rpc(RpcRegistry& rpc, std::function<meeting::DictationService*()> service);

}  // namespace wb