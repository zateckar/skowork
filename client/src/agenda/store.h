// client/agenda storage in the encrypted app.db (add-agenda design D4): Agenda items, their sources, the detection
// queue, and the Agenda's settings and counters.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "agenda/core.h"
#include "base/json.h"
#include "base/result.h"
#include "storage/database.h"

namespace wb::agenda {

struct ItemSource {
    std::string kind, source_id, conversation, title, quote, link;
    long long at = 0;
};

struct Item {
    std::string id, title, action;
    long long due_at = 0;
    bool has_time = false, inferred = false;
    std::string state = "open";  // open | done | dismissed
    std::string draft_kind = "none";
    bool user_edited = false;
    std::string prep_thread_id, prep_run_id;
    long long prepared_at = 0;
    bool refreshed = false;
    std::vector<std::string> reminders;  // reminder keys already shown
    std::string notice;                  // a later source revision the user should see ("date changed")
    long long created_at = 0, updated_at = 0, state_at = 0;
    std::vector<ItemSource> sources;
};

struct Pending {
    SourceItem item;
    long long queued_at = 0;
    int attempts = 0;
    long long next_at = 0;
};

json to_json(const Item& item);
json to_json(const SourceItem& item);
std::optional<SourceItem> source_item_from(const json& j);

class Store {
public:
    explicit Store(Database& db) : db_(db) {}

    Status put(const Item& item);  // the item and its sources, replacing both
    std::optional<Item> get(const std::string& id);
    std::vector<Item> list(const std::string& state = {});  // by due time; all states when empty
    std::vector<Item> open_between(long long from_ms, long long to_ms);
    std::vector<std::string> items_with_source(const std::string& kind, const std::string& source_id);
    Status remove_source(const std::string& kind, const std::string& source_id);  // and items left without a source
    Status remove(const std::string& id);
    // Items in done or dismissed whose state changed before `before_ms`.
    int purge_closed(long long before_ms);
    Status clear();

    // The detection queue (design D1): at most `cap` items; the oldest go first when it is full. Returns how many were
    // dropped to make room.
    int enqueue(const SourceItem& item, long long now_ms, size_t cap);
    std::vector<Pending> due_pending(long long now_ms, size_t limit);
    Status dequeue(const std::vector<SourceItem>& items);
    Status retry_later(const std::vector<SourceItem>& items, long long now_ms);
    int pending_count();
    Status unqueue_source(const std::string& kind, const std::string& source_id);

    std::optional<std::string> state(const std::string& key);
    Status set_state(const std::string& key, const std::string& value);
    long long counter(const std::string& key);
    Status add_counter(const std::string& key, long long delta);

private:
    std::vector<ItemSource> sources_of(const std::string& id);
    Database& db_;
};

}  // namespace wb::agenda
