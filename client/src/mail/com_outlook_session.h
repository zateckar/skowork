// OutlookSession over the classic Outlook object model (client/mail-calendar Local Outlook Backend, Outlook Availability,
// Sending Through Outlook; add-local-outlook-and-teams design D2, D3). All calls run on one below-normal-priority STA
// thread through late-bound IDispatch. It attaches to a running Outlook (GetActiveObject) and starts Outlook only when
// allowed. Busy Outlook (a modal dialog) is retried with backoff; a call that takes longer than 60 s answers "offline".
// Windows only. Not exercised on the development machine (no Outlook): spike 1.1 validates it.
#pragma once

#include "mail/outlook_session.h"

#include <functional>
#include <memory>

namespace wb::mail::outlook {

// Whether classic Outlook is registered for the user (ProgID Outlook.Application). New Outlook registers none.
bool outlook_installed();

class ComOutlookSession final : public OutlookSession {
public:
    // `may_start`: the user allows Skowork to start Outlook when it is not running (default off).
    explicit ComOutlookSession(std::function<bool()> may_start = {});
    ~ComOutlookSession() override;
    ComOutlookSession(const ComOutlookSession&) = delete;
    ComOutlookSession& operator=(const ComOutlookSession&) = delete;

    // running | not_running | busy (for the settings section); probes Outlook on the worker thread.
    std::string state() override;

    Result<OlAddress> me() override;
    Result<std::vector<OlFolder>> folders() override;
    Result<std::vector<OlRow>> rows(const std::string& folder_id, long long received_since) override;
    Result<OlMessage> message(const std::string& id) override;
    Result<std::vector<OlMessage>> find(const MessageQuery& q) override;
    Result<std::vector<OlAttachment>> attachments(const std::string& message_id) override;
    Result<std::string> attachment_bytes(const std::string& message_id, const std::string& attachment_id) override;
    Result<OlMessage> create_reply(const std::string& id, const std::string& kind, const std::string& comment) override;
    Result<OlMessage> create_draft(const DraftPatch& p) override;
    Result<OlMessage> update_draft(const std::string& id, const DraftPatch& p) override;
    Status send(const std::string& draft_id) override;
    Status send_new(const DraftPatch& p) override;
    Result<OlMessage> move(const std::string& id, const std::string& folder_id) override;
    Status remove(const std::string& id) override;
    Result<std::vector<OlEvent>> events(long long from, long long to, size_t limit) override;
    Result<std::string> calendar_stamp() override;
    Result<std::string> folder_stamp(const std::string& folder_id) override;
    Result<OlEvent> event(const std::string& id) override;
    Result<OlEvent> save_event(const std::string& id, const EventPatch& p) override;
    Status respond(const std::string& id, const std::string& response, const std::string& comment) override;
    Result<std::string> free_busy(const std::string& address, long long from, int interval_minutes, int slots) override;
    Status display(const std::string& id) override;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace wb::mail::outlook
