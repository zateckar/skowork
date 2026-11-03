// The work inbox in the host (client/work-inbox; add-work-inbox design D1, D6, D8-D12): it keeps the inbox state of
// Threads in step with Run events, decides read state and notifications from the focused Thread, routes standalone
// Action items to the capabilities that raised them, sends composer messages with their attachments, and serves the
// single-page app's inbox API. Run events are processed in order on the service's own thread, so the runtime's
// threads never wait for it.
#pragma once

#include "host/rpc.h"
#include "inbox/inbox_store.h"
#include "runtime/runtime.h"
#include "tools/files.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace wb::inbox {

// The notification kinds (Desktop Notifications), each with a setting and a policy key of the same name.
inline constexpr const char* kNotificationKinds[] = {"completed", "failed", "action"};

struct InboxEnv {
    runtime::AgentRuntime* runtime = nullptr;
    // Sends an event to the single-page app when the window is open (any thread).
    std::function<void(const std::string& event, const json& data)> emit;
    // Shows a toast (any thread); clicking it opens the Thread.
    std::function<void(const std::string& title, const std::string& body, const std::string& thread_id)> notify;
    // The value the Policy bundle in force sets explicitly for `key`, if any (managed settings).
    std::function<std::optional<json>(const std::string& key)> policy_value;
    // Document text extraction (client/core-tools) for composer attachments; null refuses documents.
    tools::Extractor extract;
    // The UI language for host-side texts (toasts): "cs", "de" or "en".
    std::function<std::string()> language;
    // The forget cascade of client/local-storage; null: the inbox deletes the Thread itself.
    std::function<Status(const std::string& entity_type, const std::string& id)> forget;
    // The usable Skills for the composer's `/` list, [{name, description}]; null: none.
    std::function<json()> skills;
    const Clock* clock = nullptr;  // default: the system clock
    // Whether Chats are listed as Inbox entries (policy.messaging.mode is not off); null: they are.
    std::function<bool()> chats_listed;
};

class InboxService {
public:
    InboxService(Database& db, InboxEnv env);
    ~InboxService();
    InboxService(const InboxService&) = delete;
    InboxService& operator=(const InboxService&) = delete;

    // A Run event in the JSON form of AgentService ({kind, run_id, thread_id, seq, at, data}); returns at once.
    void on_event(const json& event);
    // Waits until every event handed over so far is processed (tests).
    void wait_idle();

    // The Thread the user is looking at: open in the window, and the window has the focus. Empty: none.
    void set_focus(const std::string& thread_id, bool window_focused);
    [[nodiscard]] std::string focused_thread();
    [[nodiscard]] std::string open_thread();  // the Thread the window shows, focused or not

    // ---- Standalone Action items (design D9) ----
    using AnswerHandler = std::function<void(const runtime::ActionItem& item, const std::string& answer)>;
    void register_capability(const std::string& capability, AnswerHandler handler);
    // Raises a Question with fixed choices in `thread_id`, or in a new Thread with origin system when it is empty.
    Result<runtime::ActionItem> raise(const std::string& capability, const std::string& prompt,
                                      const std::vector<std::string>& choices, const std::string& thread_id = {},
                                      const std::string& copy = {});

    // ---- Composer (design D12) ----
    struct SendOutcome {
        std::string thread_id, run_id;
        long long queued_message = 0;
    };
    // Sends the draft of `thread_key` ("new" or a Thread id) with `text`, converting its attachments first.
    Result<SendOutcome> send(const std::string& thread_key, const std::string& text, runtime::MessageMode mode);

    // ---- Settings (Notifications, Localization) ----
    // {key: {value, managed}} for every inbox setting.
    [[nodiscard]] json settings();
    Status set_setting(const std::string& key, const json& value);
    [[nodiscard]] bool notification_enabled(const std::string& kind);

    // The inbox handler of the forget cascade for entity type "thread" (Delete With Confirmation): ends a queued or
    // interrupted Run of the Thread, then removes it; fails with "thread_busy" while a Run is running.
    Status delete_thread(const std::string& thread_id);

    void register_rpc(RpcRegistry& rpc, bool expose_on_pipe);
    // Development builds: dev.raise_action and dev.action_answers with the test capability "dev.test".
    void register_dev_rpc(RpcRegistry& rpc);
    InboxStore& store() { return store_; }
    // A Trigger Occurrence ended (client/scheduler): "completed" or "failed" for the Thread, under the same
    // notification policy as other Runs. The scheduler decides when (a Trigger's notify setting, only the final attempt).
    void notify_outcome(const std::string& kind, const std::string& thread_id, const std::string& reason);
    // A Chat or its Inbox entry changed (client/user-messaging; add-chats-in-inbox design D1): inbox.changed with the
    // entry, or with an empty id when the whole list may have changed.
    void chat_changed(const std::string& chat_id);

private:
    void worker();
    void process(const json& event);
    void changed(const std::string& thread_id);
    void notify(const std::string& kind, const std::string& thread_id, const json& data);
    bool is_focused(const std::string& thread_id);
    json entry_json(const ThreadEntry& e);

    InboxEnv env_;
    InboxStore store_;

    std::mutex focus_mx_;
    std::string focus_thread_;
    bool window_focused_ = false;

    std::mutex cap_mx_;
    std::map<std::string, AnswerHandler> capabilities_;
    json dev_answers_ = json::array();

    std::mutex q_mx_;
    std::condition_variable q_cv_;
    std::deque<json> queue_;
    bool busy_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

// The first line of a message, at most 80 characters (design D8).
std::string title_from(const std::string& text);

json to_json(const ThreadEntry& e);
json to_json(const Counts& c);

}  // namespace wb::inbox
