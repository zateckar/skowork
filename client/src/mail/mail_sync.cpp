#include "mail/mail_sync.h"

#include "base/strings.h"
#include "base/url.h"
#include "mail/graph_parse.h"
#include "platform/log.h"

#include <set>

namespace wb::mail {
namespace {

constexpr char kLog[] = "mail";
constexpr long long kDay = 86'400'000;
constexpr int kDeltaPageSize = 50;

}  // namespace

bool own_mail_folder(const StoredFolder& f) {
    const std::string w = to_lower(f.well_known);
    return w == "drafts" || w == "sentitems" || w == "outbox";
}

MailSync::MailSync(MailStore& store, const Clock& clock) : store_(store), clock_(clock) {}

void MailSync::set_config(const MailConfig& cfg) {
    const std::lock_guard lock(cfg_mx_);
    cfg_ = cfg;
}

Rules MailSync::rules() {
    MailConfig cfg;
    {
        const std::lock_guard lock(cfg_mx_);
        cfg = cfg_;
    }
    return make_rules(cfg, store_.user_exclusions("folder"), store_.user_exclusions("category"));
}

void MailSync::on_event(std::function<void(const MailEvent&)> fn) {
    const std::lock_guard lock(cfg_mx_);
    listeners_.push_back(std::move(fn));
}

void MailSync::publish(const MailEvent& e, SyncReport& r) {
    std::vector<std::function<void(const MailEvent&)>> listeners;
    {
        const std::lock_guard lock(cfg_mx_);
        listeners = listeners_;
    }
    ++r.published;
    for (const auto& fn : listeners) {
        try {
            fn(e);
        } catch (const std::exception& ex) {
            log::warn(kLog, std::string("mail event listener: ") + ex.what());
        }
    }
}

Result<SyncReport> MailSync::sync_once(GraphClient& graph, const std::shared_ptr<CancelToken>& cancel) {
    const std::lock_guard cycle(mx_);
    graph_ = &graph;
    MailConfig cfg;
    {
        const std::lock_guard lock(cfg_mx_);
        cfg = cfg_;
    }
    SyncReport r;
    if (cfg.mail_enabled) {
        GraphCall c;
        c.path = "/me/mailFolders?$top=100";
        c.cancel = cancel;
        auto list = graph_->list(c, 500);
        if (!list) return list.error();
        std::vector<StoredFolder> folders;
        for (const auto& f : *list) {
            StoredFolder sf{jstr(f, "id"), jstr(f, "displayName"), jstr(f, "wellKnownName")};
            if (!sf.id.empty()) folders.push_back(std::move(sf));
        }
        store_.set_folders(folders);
        const Rules rl = rules();
        for (const auto& f : folders) {
            if (folder_excluded(rl, {f.well_known, f.name})) {
                // An excluded folder is not synced; what was synced before is dropped without events.
                for (const auto& id : store_.message_ids(f.id)) store_.remove_message(id);
                store_.drop_sync_state("folder:" + f.id);
                continue;
            }
            ++r.folders;
            if (Status s = sync_folder(f, r, cancel); !s) return s.error();
        }
    }
    if (cfg.calendar_enabled)
        if (Status s = sync_calendar(r, cancel); !s) return s.error();
    const long long now = clock_.unix_millis();
    store_.prune(now - cfg.window.mail_days * kDay, now - cfg.window.calendar_past_days * kDay, now + cfg.window.calendar_future_days * kDay);
    return r;
}

Status MailSync::sync_folder(const StoredFolder& f, SyncReport& r, const std::shared_ptr<CancelToken>& cancel) {
    const std::string key = "folder:" + f.id;
    MailConfig cfg;
    {
        const std::lock_guard lock(cfg_mx_);
        cfg = cfg_;
    }
    const Rules rl = rules();
    const long long cutoff = clock_.unix_millis() - cfg.window.mail_days * kDay;
    const bool own = own_mail_folder(f);
    for (int round = 0; round < 2; ++round) {
        std::string link = store_.delta_link(key);
        const bool full = link.empty();
        // A folder synced for the first time only fills the store: its old mail is not "received" now.
        const bool backfill = full && store_.sync_extra(key).empty();
        if (full)
            link = "/me/mailFolders/" + url_encode(f.id) + "/messages/delta?$filter=" +
                   url_encode("receivedDateTime ge " + iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(cutoff)))) +
                   "&$expand=" + url_encode(label_expand());
        std::set<std::string> seen;
        bool expired = false;
        while (!link.empty()) {
            GraphCall c;
            c.path = link;
            c.cancel = cancel;
            c.page_size = kDeltaPageSize;
            auto p = graph_->page(c);
            if (!p) {
                if (p.error().code == "sync_state_not_found" && round == 0) {
                    log::info(kLog, "delta token expired for a folder; full resync");
                    store_.set_delta_link(key, "", store_.last_sync(), "synced");
                    ++r.resyncs;
                    expired = true;
                    break;
                }
                return p.error();
            }
            for (const auto& item : p->items) {
                const std::string id = jstr(item, "id");
                if (id.empty()) continue;
                if (item.contains("@removed")) {
                    if (auto old = store_.remove_message(id); old && !old->withheld)
                        publish({"mail.deleted", id, {{"id", id}, {"folder", f.well_known.empty() ? f.name : f.well_known}}}, r);
                    continue;
                }
                Disposition d = Disposition::include;
                auto m = parse_message(item, rl, {f.well_known, f.name}, &d);
                if (!m) continue;
                if (m->folder_id.empty()) m->folder_id = f.id;
                seen.insert(id);
                if (d == Disposition::excluded || (m->received_at && m->received_at < cutoff)) {
                    store_.remove_message(id);
                    continue;
                }
                ++r.messages;
                const auto change = store_.upsert_message(*m);
                if (change == MailStore::Change::none || m->withheld) continue;
                json detail = message_json(*m, false);
                detail["folder"] = f.well_known.empty() ? f.name : f.well_known;
                if (change == MailStore::Change::created && !own && !m->is_draft) {
                    MailEvent e{"mail.received", id, std::move(detail)};
                    if (backfill) e.detail["backfill"] = true;
                    publish(e, r);
                } else if (change == MailStore::Change::updated) {
                    publish({"mail.updated", id, std::move(detail)}, r);
                }
            }
            if (!p->delta_link.empty()) {
                if (full) {
                    // A full sync is complete: what the store has and Graph did not list is gone.
                    for (const auto& id : store_.message_ids(f.id))
                        if (!seen.count(id))
                            if (auto old = store_.remove_message(id); old && !old->withheld && !backfill)
                                publish({"mail.deleted", id, {{"id", id}, {"folder", f.well_known.empty() ? f.name : f.well_known}}}, r);
                }
                store_.set_delta_link(key, p->delta_link, clock_.unix_millis(), "synced");
                return {};
            }
            link = p->next_link;
        }
        if (!expired) return {};
    }
    return make_error("sync_state_not_found", "the folder could not be resynchronised");
}

Status MailSync::sync_calendar(SyncReport& r, const std::shared_ptr<CancelToken>& cancel) {
    const std::string key = "calendar";
    MailConfig cfg;
    {
        const std::lock_guard lock(cfg_mx_);
        cfg = cfg_;
    }
    const Rules rl = rules();
    const long long now = clock_.unix_millis();
    // The window is fixed when a delta sequence starts; a new day starts a new sequence (deduplicated against the store).
    const long long day = now / kDay;
    const long long from = (day - cfg.window.calendar_past_days) * kDay;
    const long long to = (day + 1 + cfg.window.calendar_future_days) * kDay;
    const std::string window = std::to_string(from) + "/" + std::to_string(to);
    for (int round = 0; round < 2; ++round) {
        std::string link = store_.delta_link(key);
        const std::string previous = store_.sync_extra(key);
        if (previous != window) link.clear();
        const bool full = link.empty();
        const bool backfill = full && previous.empty();
        if (full)
            link = "/me/calendarView/delta?startDateTime=" + url_encode(iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(from)))) +
                   "&endDateTime=" + url_encode(iso8601_utc(sys_clock::time_point(std::chrono::milliseconds(to)))) +
                   "&$expand=" + url_encode(label_expand());
        std::set<std::string> seen;
        bool expired = false;
        while (!link.empty()) {
            GraphCall c;
            c.path = link;
            c.cancel = cancel;
            c.page_size = kDeltaPageSize;
            auto p = graph_->page(c);
            if (!p) {
                if (p.error().code == "sync_state_not_found" && round == 0) {
                    log::info(kLog, "calendar delta token expired; full resync");
                    store_.set_delta_link(key, "", store_.last_sync(), "expired");
                    ++r.resyncs;
                    expired = true;
                    break;
                }
                return p.error();
            }
            for (const auto& item : p->items) {
                const std::string id = jstr(item, "id");
                if (id.empty()) continue;
                if (item.contains("@removed")) {
                    if (auto old = store_.remove_event(id); old && !old->withheld && !old->masked && !old->cancelled)
                        publish({"event.cancelled", id, event_json(*old, false), old->start_at, old->end_at}, r);
                    continue;
                }
                Disposition d = Disposition::include;
                auto e = parse_event(item, rl, &d);
                if (!e) continue;
                seen.insert(id);
                if (d == Disposition::excluded) {
                    store_.remove_event(id);
                    continue;
                }
                const auto before = store_.event(id);
                ++r.events;
                const auto change = store_.upsert_event(*e);
                if (change == MailStore::Change::none || e->withheld || e->masked) continue;
                MailEvent ev{"event.updated", id, event_json(*e, false), e->start_at, e->end_at};
                if (e->cancelled && !(before && before->cancelled)) ev.kind = "event.cancelled";
                else if (change == MailStore::Change::created) ev.kind = "event.created";
                if (backfill && change == MailStore::Change::created) ev.detail["backfill"] = true;
                publish(ev, r);
            }
            if (!p->delta_link.empty()) {
                if (full)
                    for (const auto& id : store_.event_ids())
                        if (!seen.count(id))
                            if (auto old = store_.remove_event(id); old && !old->withheld && !old->masked && !old->cancelled && !backfill &&
                                                                    old->start_at < to && old->end_at > from)
                                publish({"event.cancelled", id, event_json(*old, false), old->start_at, old->end_at}, r);
                store_.set_delta_link(key, p->delta_link, clock_.unix_millis(), window);
                return {};
            }
            link = p->next_link;
        }
        if (!expired) return {};
    }
    return make_error("sync_state_not_found", "the calendar could not be resynchronised");
}

}  // namespace wb::mail
