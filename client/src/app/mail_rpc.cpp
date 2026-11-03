#include "app/mail_rpc.h"

namespace wb {

void register_mail_rpc(RpcRegistry& rpc, std::function<mail::MailService*()> service) {
    const auto svc = [service = std::move(service)]() -> Result<mail::MailService*> {
        mail::MailService* s = service ? service() : nullptr;
        if (!s) return make_error("unavailable", "mail is not running");
        return s;
    };
    rpc.add("mail.status", RpcExposure::ui, [svc](const json&) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        return (*s)->status();
    });
    rpc.add("mail.connect", RpcExposure::ui, [svc](const json&) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        if (Status st = (*s)->connect(); !st) return st.error();
        return json::object();
    });
    rpc.add("mail.disconnect", RpcExposure::ui, [svc](const json&) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        (*s)->disconnect();
        return json::object();
    });
    rpc.add("mail.sync_now", RpcExposure::ui, [svc](const json&) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        (*s)->request_sync();
        return json::object();
    });
    rpc.add("mail.exclusions.add", RpcExposure::ui, [svc](const json& p) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        if (Status st = (*s)->add_exclusion(jstr(p, "kind"), jstr(p, "value")); !st) return st.error();
        return json::object();
    });
    rpc.add("mail.exclusions.remove", RpcExposure::ui, [svc](const json& p) -> Result<json> {
        auto s = svc();
        if (!s) return s.error();
        if (Status st = (*s)->remove_exclusion(jstr(p, "kind"), jstr(p, "value")); !st) return st.error();
        return json::object();
    });
}

}  // namespace wb
