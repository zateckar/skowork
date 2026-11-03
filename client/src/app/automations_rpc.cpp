#include "app/automations_rpc.h"

#include "scheduler/icu_zone.h"

#include <cstdio>
#include <memory>

namespace wb {

void register_automations_rpc(RpcRegistry& rpc, AutomationsRpcEnv env) {
    auto e = std::make_shared<AutomationsRpcEnv>(std::move(env));
    const auto unavailable = [] { return make_error("unavailable", "the scheduler is not running"); };
    rpc.add("automations.list", RpcExposure::ui, [e, unavailable](const json&) -> Result<json> {
        scheduler::Scheduler* s = e->scheduler();
        if (!s) return unavailable();
        const std::string zone_id = e->local_zone ? e->local_zone() : std::string("UTC");
        const auto zone = scheduler::icu_zone(zone_id);
        json remote = json::array();
        if (e->db) {
            auto q = e->db->prepare("SELECT task_id, issuer_name, state, thread_id, note, created_at FROM remote_tasks ORDER BY created_at DESC LIMIT 50");
            while (q.step()) {
                const scheduler::LocalTime l = zone ? (*zone)->local(q.col_int(5)) : scheduler::FixedZone().local(q.col_int(5));
                char buf[32];
                std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d", l.year, l.month, l.day, l.hour, l.minute);
                remote.push_back({{"task_id", q.col_text(0)}, {"issuer_name", q.col_text(1)}, {"state", q.col_text(2)},
                                  {"thread_id", q.col_text(3)}, {"note", q.col_text(4)}, {"created_text", buf}});
            }
        }
        return json{{"triggers", s->list()}, {"kinds", s->available_kinds()}, {"local_zone", zone_id}, {"remote", remote}};
    });
    rpc.add("automations.get", RpcExposure::ui, [e, unavailable](const json& p) -> Result<json> {
        scheduler::Scheduler* s = e->scheduler();
        if (!s) return unavailable();
        auto t = s->get(jstr(p, "id"));
        if (!t) return t.error();
        if (e->first_message_seq)
            for (auto& o : (*t)["history"])
                if (const std::string run = jstr(o, "run_id"); !run.empty())
                    if (const auto seq = e->first_message_seq(run)) o["msg_seq"] = *seq;
        return t;
    });
    rpc.add("automations.save", RpcExposure::ui, [e, unavailable](const json& p) -> Result<json> {
        scheduler::Scheduler* s = e->scheduler();
        if (!s) return unavailable();
        const json def = p.contains("definition") ? p["definition"] : json::object();
        const std::string id = jstr(p, "id");
        return id.empty() ? s->create(def, "user") : s->update(id, def);
    });
    rpc.add("automations.delete", RpcExposure::ui, [e, unavailable](const json& p) -> Result<json> {
        scheduler::Scheduler* s = e->scheduler();
        if (!s) return unavailable();
        if (Status st = s->remove(jstr(p, "id")); !st) return st.error();
        return json::object();
    });
    rpc.add("automations.run_now", RpcExposure::ui, [e, unavailable](const json& p) -> Result<json> {
        scheduler::Scheduler* s = e->scheduler();
        if (!s) return unavailable();
        return s->run_now(jstr(p, "id"));
    });
}

}  // namespace wb
