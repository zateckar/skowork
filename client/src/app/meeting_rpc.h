// The Meetings settings section's host methods (client/meeting-capture Meetings Settings Section), UI only:
// meetings.status, .sources, .stop, .capture, .transcribe_now, .rename_speaker, .restore_echo, .add_term, .remove_term, .remove_series and .forget.
#pragma once

#include "host/rpc.h"
#include "meeting/dictation.h"
#include "meeting/meeting_service.h"
#include "meeting/speech_installer.h"

#include <functional>

namespace wb {

// `service`: the running meeting service, or null while it is not running.
void register_meeting_rpc(RpcRegistry& rpc, std::function<meeting::MeetingService*()> service);

// client/dictation (add-dictation D1): dictation.start, .stop and .cancel, UI only.
void register_dictation_rpc(RpcRegistry& rpc, std::function<meeting::DictationService*()> service);

// client/speech-models (tasks 5.2, 5.3): speech.status, .install (download from the built-in source), .import {path},
// .cancel and .remove {version}, UI only. Settings (task 5.4) calls these; no URL is accepted from the caller.
void register_speech_rpc(RpcRegistry& rpc, std::function<meeting::SpeechInstaller*()> service);

}  // namespace wb