// The inbox harness shared by the inbox and window tests: the scripted runtime plus the inbox service fed with its
// events, as in the app. Each test file gets its own copy (anonymous namespace).
#pragma once

#include "app/agent_service.h"
#include "inbox/inbox_service.h"
#include "support/runtime_harness.h"

#include <map>
#include <mutex>

namespace {

using wb::inbox::InboxEnv;
using wb::inbox::InboxService;

struct Toast {
    std::string title, body, thread_id;
};

// The scripted runtime plus the inbox service fed with its events, as in the app.
struct Inbox {
    explicit Inbox(std::function<void(InboxEnv&)> configure = nullptr) : configure_(std::move(configure)) { make(); }
    ~Inbox() { svc.reset(); }

    void make() {
        svc.reset();
        InboxEnv e;
        e.runtime = env.rt.get();
        e.clock = &env.clock;
        e.emit = [this](const std::string& ev, const json& d) {
            {
                const std::lock_guard lock(mx);
                emitted.emplace_back(ev, d);
            }
            if (on_emit) on_emit(ev, d);
        };
        e.notify = [this](const std::string& title, const std::string& body, const std::string& thread) {
            const std::lock_guard lock(mx);
            toasts.push_back({title, body, thread});
        };
        e.policy_value = [this](const std::string& key) -> std::optional<json> {
            const std::lock_guard lock(mx);
            if (const auto it = policy.find(key); it != policy.end()) return it->second;
            return std::nullopt;
        };
        if (configure_) configure_(e);
        svc = std::make_unique<InboxService>(*env.db, std::move(e));
        env.rt->set_listener([this](const RunEvent& ev) {
            env.events(ev);
            const json j = {{"kind", ev.kind}, {"run_id", ev.run_id}, {"thread_id", ev.thread_id}, {"seq", ev.seq},
                            {"at", ev.at}, {"data", ev.data}};
            svc->on_event(j);
            if (on_emit) on_emit("run.event", j);
        });
    }
    // A restart of the application: a new runtime and a new service on the same app.db.
    void restart() {
        svc.reset();
        env.restart();
        make();
    }
    std::string run_to_end(const std::string& thread, const std::string& text, const std::string& answer = "done") {
        env.model.push_text(answer);
        const auto id = env.run(thread, text);
        EXPECT_TRUE(env.wait(id, RunState::succeeded));
        svc->wait_idle();
        return id;
    }
    wb::inbox::ThreadEntry entry(const std::string& id) { return *svc->store().entry(id); }
    std::vector<std::string> view(const std::string& v) {
        std::vector<std::string> out;
        std::string cursor;
        do {
            auto page = svc->store().list(v, cursor, 7);
            EXPECT_TRUE(page);
            if (!page) break;
            for (const auto& e : page->entries) out.push_back(e.id);
            cursor = page->next_cursor;
        } while (!cursor.empty());
        return out;
    }
    bool in_view(const std::string& v, const std::string& id) {
        const auto ids = view(v);
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }
    std::vector<Toast> toast_list() {
        const std::lock_guard lock(mx);
        return toasts;
    }

    Env env;
    std::function<void(InboxEnv&)> configure_;
    std::unique_ptr<InboxService> svc;
    std::mutex mx;
    std::vector<std::pair<std::string, json>> emitted;
    std::vector<Toast> toasts;
    std::map<std::string, json> policy;
    // Also receives every emitted event (the window tests forward them to the window), on the emitting thread.
    std::function<void(const std::string&, const json&)> on_emit;
};

}  // namespace
