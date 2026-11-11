#include "app/meeting_rpc.h"

namespace wb {

void register_meeting_rpc(RpcRegistry& rpc, std::function<meeting::MeetingService*()> service) {
    const auto svc = [service = std::move(service)]() -> Result<meeting::MeetingService*> {
        meeting::MeetingService* s = service ? service() : nullptr;
        if (!s) return make_error("unavailable", "meeting capture is not running");
        return s;
    };
    const auto done = [](const Status& st) -> Result<json> {
        if (!st) return st.error();
        return json::object();
    };
    rpc.add("meetings.status", RpcExposure::ui, [svc](const json&) -> Result<json> {
        auto s = svc();
        if (!s) return json{{"available", false}};
        return (*s)->status();
    });
    rpc.add("meetings.sources", RpcExposure::ui, [svc](const json&) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return json{{"sources", (*s)->audio_sources()}};
    });
    rpc.add("meetings.stop", RpcExposure::ui, [svc, done](const json&) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return done((*s)->stop_capture());
    });
    rpc.add("meetings.capture", RpcExposure::ui, [svc, done](const json& p) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return done((*s)->capture_manual(jstr(p, "source")));
    });
    rpc.add("meetings.transcribe_now", RpcExposure::ui, [svc, done](const json& p) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return done((*s)->transcribe_now(jstr(p, "id")));
    });
    rpc.add("meetings.rename_speaker", RpcExposure::ui, [svc, done](const json& p) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return done((*s)->rename_speaker(jstr(p, "id"), jstr(p, "speaker"), jstr(p, "name")));
    });
    rpc.add("meetings.restore_echo", RpcExposure::ui, [svc, done](const json& p) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return done((*s)->restore_echo(jstr(p, "id"), jint(p, "start_ms", -1)));
    });
    rpc.add("meetings.add_term", RpcExposure::ui, [svc, done](const json& p) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return done((*s)->add_term(jstr(p, "term")));
    });
    rpc.add("meetings.remove_term", RpcExposure::ui, [svc, done](const json& p) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return done((*s)->remove_term(jstr(p, "term")));
    });
    rpc.add("meetings.remove_series", RpcExposure::ui, [svc, done](const json& p) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return done((*s)->remove_series(jstr(p, "id")));
    });
    rpc.add("meetings.forget", RpcExposure::ui, [svc, done](const json& p) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return done((*s)->forget(jstr(p, "id")));
    });
}

void register_dictation_rpc(RpcRegistry& rpc, std::function<meeting::DictationService*()> service) {
    const auto svc = [service = std::move(service)]() -> Result<meeting::DictationService*> {
        meeting::DictationService* s = service ? service() : nullptr;
        if (!s) return make_error("unavailable", "dictation is not running");
        return s;
    };
    rpc.add("dictation.start", RpcExposure::ui, [svc](const json&) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return (*s)->start();
    });
    rpc.add("dictation.stop", RpcExposure::ui, [svc](const json&) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return (*s)->stop();
    });
    rpc.add("dictation.cancel", RpcExposure::ui, [svc](const json&) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        if (Status st = (*s)->cancel(); !st) return st.error();
        return json::object();
    });
}

}  // namespace wb