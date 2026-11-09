// client/work-inbox, host side (add-work-inbox tasks 2.1-2.3, 4.2, 4.3, 5.1-5.3, 6.1, 6.2): the inbox state of
// Threads driven by real Runs of the scripted runtime, the views with keyset paging, folders and pinning, standalone
// Action items, drafts across a restart, queue and steer through the composer, fork, search with diacritic folding,
// notifications and delete through the forget cascade.
#include "app/agent_service.h"
#include "host/toast.h"
#include "inbox/inbox_service.h"
#include "storage/forget.h"
#include "support/inbox_harness.h"

#include <map>

namespace {

using wb::inbox::ThreadEntry;

}  // namespace

// ==== 2.1 Thread inbox fields: Read State and Archive ===================================================================

TEST(InboxReadState, ResultInABackgroundThreadMarksItUnreadOpeningMarksItRead) {
    Inbox in;
    in.env.start();
    const auto t = in.env.thread();
    in.run_to_end(t, "summarise the report", "Here is the summary");
    ThreadEntry e = in.entry(t);
    EXPECT_TRUE(e.unread);
    EXPECT_EQ(e.preview, "Here is the summary");
    EXPECT_EQ(e.last_run_state, "succeeded");
    EXPECT_TRUE(in.in_view("unread", t));
    ASSERT_TRUE(in.svc->store().set_unread(t, false));  // threads.open
    EXPECT_FALSE(in.in_view("unread", t));
}

TEST(InboxReadState, ResultInTheFocusedThreadStaysRead) {
    Inbox in;
    in.env.start();
    const auto t = in.env.thread();
    in.svc->set_focus(t, true);
    in.run_to_end(t, "hello");
    EXPECT_FALSE(in.entry(t).unread);
    // The window lost the focus: the next result makes the Thread unread.
    in.svc->set_focus(t, false);
    in.run_to_end(t, "again");
    EXPECT_TRUE(in.entry(t).unread);
}

TEST(InboxReadState, FailedRunMarksUnreadCancelledRunDoesNot) {
    Inbox in;
    in.env.start();
    const auto t = in.env.thread();
    in.env.model.push_failure(failure(model::code::rate_limit));
    const auto failed = in.env.run(t, "x");
    ASSERT_TRUE(in.env.wait(failed, RunState::failed));
    in.svc->wait_idle();
    EXPECT_TRUE(in.entry(t).unread);
    ASSERT_TRUE(in.svc->store().set_unread(t, false));
    Gate gate;
    in.env.model.push_blocking(gate);
    const auto cancelled = in.env.run(t, "y");
    ASSERT_TRUE(in.env.wait(cancelled, RunState::running));
    ASSERT_TRUE(in.env.rt->cancel(cancelled));
    ASSERT_TRUE(in.env.wait(cancelled, RunState::cancelled));
    in.svc->wait_idle();
    EXPECT_FALSE(in.entry(t).unread);
}

TEST(InboxReadState, MarkUnreadSurvivesMovingToAnotherThread) {
    Inbox in;
    in.env.start();
    const auto a = in.env.thread(), b = in.env.thread();
    in.svc->set_focus(a, true);
    ASSERT_TRUE(in.svc->store().set_unread(a, true));
    in.svc->set_focus(b, true);
    EXPECT_TRUE(in.entry(a).unread);
    EXPECT_TRUE(in.in_view("unread", a));
}

TEST(InboxArchive, ArchiveLeavesAllAndUnreadAndRestoreBringsItBack) {
    Inbox in;
    in.env.start();
    const auto t = in.env.thread();
    in.run_to_end(t, "work");
    ASSERT_TRUE(in.svc->store().set_archived(t, true));
    EXPECT_FALSE(in.in_view("all", t));
    EXPECT_FALSE(in.in_view("unread", t));
    EXPECT_TRUE(in.in_view("archive", t));
    ASSERT_TRUE(in.svc->store().set_archived(t, false));
    EXPECT_TRUE(in.in_view("all", t));
    EXPECT_FALSE(in.in_view("archive", t));
}

TEST(InboxArchive, NewRunOrActionItemUnarchivesBackIntoItsFolder) {
    Inbox in;
    in.env.add_tool(mail_tool(in.env));
    in.env.policy->approvals.insert("mail.send");
    in.env.start();
    const auto t = in.env.thread();
    auto folder = in.svc->store().create_folder("Project X");
    ASSERT_TRUE(folder);
    ASSERT_TRUE(in.svc->store().move(t, folder->id));
    ASSERT_TRUE(in.svc->store().set_archived(t, true));
    in.run_to_end(t, "a new Run");  // as a Trigger appending a Run would
    EXPECT_FALSE(in.entry(t).archived);
    EXPECT_TRUE(in.in_view("folder:" + folder->id, t));
    EXPECT_TRUE(in.in_view("all", t));

    ASSERT_TRUE(in.svc->store().set_archived(t, true));
    auto a = in.svc->raise("dev.test", "Connect your mailbox", {"Connect", "Not now"}, t);
    ASSERT_TRUE(a);
    EXPECT_FALSE(in.entry(t).archived);
}

// ==== 2.2 Views with keyset paging and counts ==========================================================================

TEST(InboxViews, KeysetPagingReturnsEveryThreadOnceNewestFirst) {
    Inbox in;
    std::vector<std::string> made;
    for (int i = 0; i < 23; ++i) {
        made.push_back(in.env.thread());
        in.env.clock.advance(1s);
    }
    const auto all = in.view("all");
    ASSERT_EQ(all.size(), made.size());
    EXPECT_EQ(all, std::vector<std::string>(made.rbegin(), made.rend()));
    auto first = in.svc->store().list("all", {}, 100);
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->next_cursor.empty());
    EXPECT_FALSE(in.svc->store().list("nonsense"));
    EXPECT_FALSE(in.svc->store().list("all", "garbage"));
}

TEST(InboxViews, ActionAndRunningViewsWithCountsIgnoreFolders) {
    Inbox in;
    in.env.add_tool(mail_tool(in.env));
    in.env.policy->approvals.insert("mail.send");
    in.env.start();
    const auto filed = in.env.thread(), plain = in.env.thread(), busy = in.env.thread();
    auto folder = in.svc->store().create_folder("Project X");
    ASSERT_TRUE(folder);
    ASSERT_TRUE(in.svc->store().move(filed, folder->id));

    // A filed Thread gets an Approval: it is in the Action view and in its folder.
    in.env.model.push_calls({{"c1", "find_tools", {{"query", "mail send"}}}});
    in.env.model.push_calls({{"c2", "mail.send", {{"to", "a@b.cz"}, {"rationale", "asked"}}}});
    const auto r = in.env.run(filed, "send it");
    ASSERT_TRUE(in.env.wait(r, RunState::interrupted));
    in.svc->wait_idle();
    EXPECT_TRUE(in.in_view("action", filed));
    EXPECT_TRUE(in.in_view("folder:" + folder->id, filed));
    EXPECT_FALSE(in.in_view("inbox", filed));
    EXPECT_TRUE(in.in_view("inbox", plain));
    EXPECT_FALSE(in.in_view("action", plain));
    EXPECT_TRUE(in.entry(filed).has_action);

    Gate gate;
    in.env.model.push_blocking(gate);
    const auto running = in.env.run(busy, "long work");
    ASSERT_TRUE(in.env.wait(running, RunState::running));
    EXPECT_EQ(in.view("running"), std::vector<std::string>{busy});
    EXPECT_TRUE(in.entry(busy).running);
    const auto c = in.svc->store().counts();
    EXPECT_EQ(c.action, 1);
    EXPECT_EQ(c.running, 1);
    gate.release();
    ASSERT_TRUE(in.env.wait(running, RunState::succeeded));
    in.svc->wait_idle();
    EXPECT_TRUE(in.view("running").empty());
    EXPECT_EQ(in.svc->store().counts().unread, 1);

    // The answered Approval takes the Thread out of the Action view.
    const auto actions = in.env.rt->pending_actions(filed);
    ASSERT_EQ(actions.size(), 1u);
    in.env.model.push_text("sent");
    ASSERT_TRUE(in.env.rt->resolve_approval(actions[0].id, ApprovalResolution::approve()));
    ASSERT_TRUE(in.env.wait(r, RunState::succeeded));
    EXPECT_FALSE(in.in_view("action", filed));
}

TEST(InboxViews, ChangeEventsCarryTheEntryAndCounts) {
    Inbox in;
    in.env.start();
    const auto t = in.env.thread();
    in.run_to_end(t, "go");
    const std::lock_guard lock(in.mx);
    ASSERT_FALSE(in.emitted.empty());
    const auto& [ev, data] = in.emitted.back();
    EXPECT_EQ(ev, "inbox.changed");
    EXPECT_EQ(jstr(data, "thread_id"), t);
    EXPECT_EQ(data["counts"]["unread"], 1);
    EXPECT_EQ(data["entry"]["id"], t);
}

// ==== 2.3 Folders and pinning ===========================================================================================

TEST(InboxFolders, CreateRenameDeleteMovesThreadsToTheInbox) {
    Inbox in;
    const auto t = in.env.thread();
    auto f = in.svc->store().create_folder("  Reports ");
    ASSERT_TRUE(f);
    EXPECT_EQ(f->name, "Reports");
    EXPECT_FALSE(in.svc->store().create_folder(" "));
    ASSERT_TRUE(in.svc->store().rename_folder(f->id, "Quarterly reports"));
    ASSERT_EQ(in.svc->store().folders().size(), 1u);
    EXPECT_EQ(in.svc->store().folders()[0].name, "Quarterly reports");
    ASSERT_TRUE(in.svc->store().move(t, f->id));
    EXPECT_EQ(in.entry(t).folder_id, f->id);
    EXPECT_FALSE(in.svc->store().move(t, "fld_missing"));
    ASSERT_TRUE(in.svc->store().delete_folder(f->id));
    EXPECT_TRUE(in.svc->store().folders().empty());
    EXPECT_TRUE(in.entry(t).folder_id.empty());
    EXPECT_TRUE(in.in_view("inbox", t));
    EXPECT_FALSE(in.svc->store().delete_folder(f->id));
}

TEST(InboxFolders, PinnedThreadsComeFirstInEveryView) {
    Inbox in;
    in.env.start();
    const auto old_one = in.env.thread();
    in.env.clock.advance(1s);
    const auto newer = in.env.thread();
    in.env.clock.advance(1s);
    in.run_to_end(old_one, "x");  // now the most recent activity...
    in.run_to_end(newer, "y");
    ASSERT_TRUE(in.svc->store().set_pinned(old_one, true));
    EXPECT_EQ(in.view("all").front(), old_one);
    EXPECT_EQ(in.view("unread").front(), old_one);
    EXPECT_TRUE(in.entry(old_one).pinned);
}

// ==== 4.2, 4.3 Questions and standalone Action items ======================================================================

TEST(InboxActions, AnsweringAQuestionResumesTheRunAndLeavesTheActionView) {
    Inbox in;
    in.env.start();
    const auto t = in.env.thread();
    in.env.model.push_calls({{"q1", "ask_user", {{"question", "Which project folder should I use?"}, {"choices", {"A", "B"}}}}});
    in.env.model.push_text("using A");
    const auto r = in.env.run(t, "organise");
    ASSERT_TRUE(in.env.wait(r, RunState::interrupted));
    in.svc->wait_idle();
    EXPECT_TRUE(in.in_view("action", t));
    const auto a = in.env.rt->pending_actions(t);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_TRUE(jbool(a[0].payload, "free_text"));
    ASSERT_TRUE(in.env.rt->answer_question(a[0].id, "the shared one"));
    ASSERT_TRUE(in.env.wait(r, RunState::succeeded));
    in.svc->wait_idle();
    EXPECT_FALSE(in.in_view("action", t));
}

TEST(InboxActions, StandaloneQuestionCreatesASystemThreadAndReturnsTheAnswerToItsCapability) {
    Inbox in;
    in.env.start();
    std::vector<std::string> answers;
    std::mutex amx;
    in.svc->register_capability("mail-calendar", [&](const ActionItem& item, const std::string& answer) {
        const std::lock_guard lock(amx);
        answers.push_back(item.raised_by + ":" + answer);
    });
    auto a = in.svc->raise("mail-calendar", "Connect your mailbox", {"Connect", "Not now"});
    ASSERT_TRUE(a) << a.error().message;
    EXPECT_TRUE(a->run_id.empty());
    const ThreadEntry e = in.entry(a->thread_id);
    EXPECT_EQ(e.origin_kind, "system");
    EXPECT_EQ(e.title, "Connect your mailbox");
    EXPECT_TRUE(e.has_action);
    EXPECT_TRUE(in.in_view("action", a->thread_id));
    EXPECT_EQ(in.env.rt->pending_actions().size(), 1u);  // the tray's Action count
    // Fixed choices only: no free text.
    EXPECT_FALSE(in.env.rt->answer_question(a->id, "Later maybe"));
    ASSERT_TRUE(in.env.rt->answer_question(a->id, "Connect"));
    {
        const std::lock_guard lock(amx);
        EXPECT_EQ(answers, std::vector<std::string>{"mail-calendar:Connect"});
    }
    EXPECT_FALSE(in.in_view("action", a->thread_id));
    EXPECT_EQ(in.env.rt->answer_question(a->id, "Not now").error().code, "already_resolved");
    EXPECT_FALSE(in.svc->raise("mail-calendar", "x", {}));
}

TEST(InboxActions, StandaloneQuestionSurvivesARestart) {
    Inbox in;
    in.env.start();
    auto a = in.svc->raise("dev.test", "Accept the Remote task?", {"Accept", "Decline"});
    ASSERT_TRUE(a);
    in.restart();
    std::string got;
    in.svc->register_capability("dev.test", [&](const ActionItem&, const std::string& answer) { got = answer; });
    const auto open = in.env.rt->pending_actions();
    ASSERT_EQ(open.size(), 1u);
    EXPECT_EQ(open[0].raised_by, "dev.test");
    ASSERT_TRUE(in.env.rt->answer_question(open[0].id, "Decline"));
    EXPECT_EQ(got, "Decline");
}

// ==== 5.1, 5.2 Composer: drafts, attachments, queue and steer ===============================================================

TEST(InboxComposer, DraftAndAttachmentsSurviveARestart) {
    Inbox in;
    in.env.start();
    const auto t = in.env.thread();
    ASSERT_TRUE(in.svc->store().save_draft_text(t, "Please review"));
    auto att = in.svc->store().add_draft_attachment(t, "notes.txt", "line one\nline two\n");
    ASSERT_TRUE(att);
    ASSERT_TRUE(in.svc->store().save_draft_text("new", "A new idea"));
    in.restart();
    const auto d = in.svc->store().draft(t);
    EXPECT_EQ(d.text, "Please review");
    ASSERT_EQ(d.attachments.size(), 1u);
    EXPECT_EQ(d.attachments[0].name, "notes.txt");
    EXPECT_EQ(d.attachments[0].size, 18);
    EXPECT_EQ(in.svc->store().draft("new").text, "A new idea");
    ASSERT_TRUE(in.svc->store().remove_draft_attachment(t, att->id));
    EXPECT_TRUE(in.svc->store().draft(t).attachments.empty());
}

TEST(InboxComposer, SendTurnsAttachmentsIntoThreadAttachmentsAndClearsTheDraft) {
    std::atomic<int> extracted{0};
    Inbox in([&](InboxEnv& e) {
        e.extract = [&](const std::string& kind, const std::string&, ToolContext&) -> Result<std::string> {
            ++extracted;
            return "text of a " + kind;
        };
    });
    in.env.start();
    ASSERT_TRUE(in.svc->store().save_draft_text("new", "ignored by send"));
    ASSERT_TRUE(in.svc->store().add_draft_attachment("new", "notes.txt", "Ahoj světe"));
    ASSERT_TRUE(in.svc->store().add_draft_attachment("new", "plan.docx", "PK\x03\x04 binary"));
    in.env.model.push_text("read them");
    auto r = in.svc->send("new", "Summarise the attached files\nplease", MessageMode::queue);
    ASSERT_TRUE(r) << r.error().message;
    ASSERT_TRUE(in.env.wait(r->run_id, RunState::succeeded));
    EXPECT_EQ(extracted.load(), 1);
    EXPECT_EQ(in.entry(r->thread_id).title, "Summarise the attached files");
    const auto conv = in.env.rt->conversation(r->thread_id);
    ASSERT_FALSE(conv.empty());
    const std::string& msg = conv[0].content;
    EXPECT_NE(msg.find("notes.txt: attachment att_"), std::string::npos) << msg;
    EXPECT_NE(msg.find("plan.docx: attachment att_"), std::string::npos) << msg;
    const auto from = msg.find("notes.txt: attachment ") + 22;
    const auto id = msg.substr(from, msg.find(',', from) - from);
    const auto info = in.env.rt->store().attachment(id);
    ASSERT_TRUE(info);
    EXPECT_EQ(info->thread_id, r->thread_id);
    EXPECT_FALSE(jbool(info->provenance, "trusted", true));
    EXPECT_EQ(*in.env.rt->store().read_attachment(id, 0, 100), "Ahoj světe");
    EXPECT_TRUE(in.svc->store().draft("new").text.empty());
    EXPECT_TRUE(in.svc->store().draft("new").attachments.empty());
}

TEST(InboxComposer, UnusableAttachmentOrBlockedStartLeavesNoThreadAndKeepsTheDraft) {
    Inbox in;
    in.env.start();
    ASSERT_TRUE(in.svc->store().add_draft_attachment("new", "image.png", std::string("\x89PNG\r\n\x1a\n\0\0\0", 11)));
    const auto before = in.view("all").size();
    auto r = in.svc->send("new", "look", MessageMode::queue);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "unsupported_attachment");
    EXPECT_EQ(in.view("all").size(), before);
    EXPECT_EQ(in.svc->store().draft("new").attachments.size(), 1u);

    Inbox blocked;
    blocked.svc.reset();
    blocked.env.cfg.start_gate = [] { return std::optional<std::string>("disconnected from the control plane"); };
    blocked.env.make_runtime();
    blocked.make();
    blocked.env.start();
    ASSERT_TRUE(blocked.svc->store().save_draft_text("new", "hello"));
    auto b = blocked.svc->send("new", "hello", MessageMode::queue);
    ASSERT_FALSE(b);
    EXPECT_TRUE(blocked.view("all").empty());
    EXPECT_EQ(blocked.svc->store().draft("new").text, "hello");
}

TEST(InboxComposer, EnterQueuesForTheNextTurnAndCtrlEnterSteersTheActiveRun) {
    Inbox in;
    in.env.start();
    const auto t = in.env.thread();
    Gate gate;
    in.env.model.push_blocking(gate, calls_response({{"c1", "find_tools", {{"query", "costs"}}}}));
    in.env.model.push_text("first turn done");
    in.env.model.push_text("second turn done");
    auto first = in.svc->send(t, "start", MessageMode::queue);
    ASSERT_TRUE(first);
    ASSERT_TRUE(in.env.wait(first->run_id, RunState::running));
    auto queued = in.svc->send(t, "then this", MessageMode::queue);
    ASSERT_TRUE(queued);
    EXPECT_TRUE(queued->run_id.empty());
    EXPECT_GT(queued->queued_message, 0);
    auto steer = in.svc->send(t, "focus on costs", MessageMode::steer_now);
    ASSERT_TRUE(steer);
    const auto pending = in.env.rt->queued_messages(t);
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending[1].mode, MessageMode::steer_now);
    gate.release();
    // The steering message reaches the active Run before its next model call; the queued one starts the next Run.
    ASSERT_TRUE(in.env.wait_for([&] { return in.env.rt->runs(t).size() == 2; }));
    const auto runs = in.env.rt->runs(t);
    ASSERT_TRUE(in.env.wait(runs[1].id, RunState::succeeded));
    EXPECT_TRUE(in.env.rt->queued_messages(t).empty());
    const auto reqs = in.env.model.requests();
    ASSERT_EQ(reqs.size(), 3u);
    bool steered = false;
    for (const auto& m : reqs[1].messages) steered |= m.role == "user" && m.content.find("focus on costs") != std::string::npos;
    EXPECT_TRUE(steered);
    bool queued_first = false;
    for (const auto& m : reqs[2].messages) queued_first |= m.role == "user" && m.content == "then this";
    EXPECT_TRUE(queued_first);
}

// ==== 5.3 Fork =============================================================================================================

TEST(InboxFork, ForkFromTheThirdMessageCopiesTheFirstThreeAndLinksBack) {
    Inbox in;
    in.env.start();
    const auto t = in.env.thread();
    in.run_to_end(t, "first question", "first answer");
    in.run_to_end(t, "second question", "second answer");
    // The message events in order; the third is "second question".
    std::vector<std::pair<std::string, long long>> msgs;
    for (const auto& r : in.env.rt->runs(t))
        for (const auto& e : in.env.rt->events(r.id))
            if (e.kind == "message") msgs.emplace_back(r.id, e.seq);
    ASSERT_EQ(msgs.size(), 4u);
    auto f = in.svc->store().fork(t, msgs[2].first, msgs[2].second);
    ASSERT_TRUE(f) << f.error().message;
    const auto conv = in.env.rt->conversation(*f);
    ASSERT_EQ(conv.size(), 3u);
    EXPECT_EQ(conv[0].content, "first question");
    EXPECT_EQ(conv[2].content, "second question");
    const ThreadEntry e = in.entry(*f);
    EXPECT_EQ(e.origin_kind, "fork");
    EXPECT_EQ(jstr(e.origin_ref, "thread_id"), t);
    EXPECT_FALSE(e.running);
    // The fork can continue on its own.
    in.run_to_end(*f, "and now differently", "a different answer");
    EXPECT_EQ(in.env.rt->conversation(*f).size(), 5u);
    EXPECT_EQ(in.env.rt->conversation(t).size(), 4u);
    EXPECT_FALSE(in.svc->store().fork(t, msgs[0].first, 999));
}

// ==== 6.1 Notifications ======================================================================================================

TEST(InboxNotifications, BackgroundCompletionFailureAndActionNotifyFocusedAndCancelledDoNot) {
    Inbox in;
    in.env.add_tool(mail_tool(in.env));
    in.env.policy->approvals.insert("mail.send");
    in.env.start();
    const auto bg = in.env.thread(), fg = in.env.thread();
    ASSERT_TRUE(in.svc->store().rename(bg, "Quarterly numbers"));
    in.svc->set_focus(fg, true);
    in.run_to_end(bg, "go", "All numbers are in");
    in.run_to_end(fg, "go");
    auto toasts = in.toast_list();
    ASSERT_EQ(toasts.size(), 1u);
    EXPECT_EQ(toasts[0].thread_id, bg);
    EXPECT_EQ(toasts[0].title, "Quarterly numbers");
    EXPECT_EQ(toasts[0].body, "Finished: All numbers are in");

    in.env.model.push_calls({{"c1", "find_tools", {{"query", "mail send"}}}});
    in.env.model.push_calls({{"c2", "mail.send", {{"to", "a@b.cz"}, {"rationale", "asked"}}}});
    const auto r = in.env.run(bg, "send it");
    ASSERT_TRUE(in.env.wait(r, RunState::interrupted));
    in.svc->wait_idle();
    toasts = in.toast_list();
    ASSERT_EQ(toasts.size(), 2u);
    EXPECT_EQ(toasts[1].body, "Approval required: mail.send");
    ASSERT_TRUE(in.env.rt->cancel(r));  // the user stops the Run: no toast
    ASSERT_TRUE(in.env.wait(r, RunState::cancelled));
    in.svc->wait_idle();
    EXPECT_EQ(in.toast_list().size(), 2u);
}

TEST(InboxNotifications, TogglesAndManagedSettings) {
    Inbox in;
    in.env.start();
    ASSERT_TRUE(in.svc->set_setting("notifications.completed", false));
    in.run_to_end(in.env.thread(), "go");
    EXPECT_TRUE(in.toast_list().empty());
    EXPECT_FALSE(in.svc->settings()["notifications.completed"]["value"].get<bool>());
    // The organization fixes the setting: it is managed and cannot be changed.
    {
        const std::lock_guard lock(in.mx);
        in.policy["notifications.completed"] = true;
    }
    const json s = in.svc->settings();
    EXPECT_TRUE(s["notifications.completed"]["managed"].get<bool>());
    EXPECT_TRUE(s["notifications.completed"]["value"].get<bool>());
    EXPECT_EQ(in.svc->set_setting("notifications.completed", false).error().code, "managed");
    in.run_to_end(in.env.thread(), "go");
    EXPECT_EQ(in.toast_list().size(), 1u);
    EXPECT_TRUE(in.svc->set_setting("ui.language", "cs"));
    EXPECT_FALSE(in.svc->set_setting("ui.language", "fr"));
    ASSERT_TRUE(in.svc->store().set_setting("dictation.language", "sk"));
    EXPECT_EQ(in.svc->settings()["dictation.language"]["value"], "auto");
    EXPECT_FALSE(in.svc->set_setting("dictation.language", "pl"));
    EXPECT_FALSE(in.svc->set_setting("dictation.language", "sk"));
    EXPECT_TRUE(in.svc->set_setting("dictation.language", "de"));
    EXPECT_EQ(in.svc->settings()["dictation.language"]["value"], "de");
    EXPECT_FALSE(in.svc->set_setting("unknown.key", true));
}

TEST(InboxNotifications, ToastTextFollowsTheLanguage) {
    Inbox in([](InboxEnv& e) { e.language = [] { return std::string("cs"); }; });
    in.env.start();
    in.env.model.push_failure(failure(model::code::timeout));
    const auto r = in.env.run(in.env.thread(), "x");
    ASSERT_TRUE(in.env.wait(r, RunState::failed));
    in.svc->wait_idle();
    const auto t = in.toast_list();
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].body.rfind("Selhalo: ", 0), 0u) << t[0].body;
}

// ==== 6.2 Search ===============================================================================================================

TEST(InboxSearch, CzechAndGermanWithAndWithoutDiacritics) {
    Inbox in;
    in.env.start();
    const auto cz = in.env.thread(), de = in.env.thread();
    ASSERT_TRUE(in.svc->store().rename(cz, "Žádost o dovolenou"));
    in.run_to_end(cz, "Připrav přehled nákladů za čtvrtletí", "Přehled je hotový");
    in.run_to_end(de, "Bitte prüfe die Größe der Datei", "Die Größe ist in Ordnung");
    ASSERT_TRUE(in.svc->store().set_archived(de, true));  // archived Threads are searched too

    const auto threads_of = [&](const std::string& q) {
        std::set<std::string> out;
        for (const auto& h : in.svc->store().search(q)) out.insert(h.thread_id);
        return out;
    };
    for (const char* q : {"náklad", "naklad", "NÁKLADŮ", "nakladu", "ctvrtleti", "čtvrtletí", "zadost", "dovolen"})
        EXPECT_EQ(threads_of(q), std::set<std::string>{cz}) << q;
    for (const char* q : {"prüfe", "prufe", "Grö", "gro"})
        EXPECT_EQ(threads_of(q), std::set<std::string>{de}) << q;
    EXPECT_TRUE(threads_of("\"").empty());
    EXPECT_TRUE(threads_of("nothing-like-this-zzz").empty());

    const auto hits = in.svc->store().search("prehled");
    ASSERT_FALSE(hits.empty());
    EXPECT_NE(hits[0].snippet.find("[["), std::string::npos);
    EXPECT_FALSE(hits[0].run_id.empty());  // jump-to-message target
    EXPECT_GT(hits[0].seq, 0);
    const auto archived = in.svc->store().search("Datei");
    ASSERT_FALSE(archived.empty());
    EXPECT_TRUE(archived[0].archived);
}

TEST(InboxSearch, RenamedTitleIsFoundAndTheOldOneIsNot) {
    Inbox in;
    const auto t = in.env.thread();
    ASSERT_TRUE(in.svc->store().rename(t, "Budget plán"));
    EXPECT_EQ(in.svc->store().search("plan").size(), 1u);
    ASSERT_TRUE(in.svc->store().rename(t, "Cestovní náhrady"));
    EXPECT_TRUE(in.svc->store().search("plan").empty());
    EXPECT_EQ(in.svc->store().search("nahrady").size(), 1u);
    EXPECT_FALSE(in.svc->store().rename(t, "   "));
}

// ==== Delete With Confirmation =====================================================================================================

TEST(InboxDelete, ForgetCascadeRemovesTheThreadItsAttachmentsAndIndexEntries) {
    ForgetRegistry forget;
    Inbox* self = nullptr;
    Inbox in([&](InboxEnv& e) { e.forget = [&](const std::string& type, const std::string& id) { return forget.forget(type, id); }; });
    self = &in;
    forget.on_forget("thread", "work-inbox", [&](const std::string& id) { return self->svc->delete_thread(id); });
    in.env.start();
    ASSERT_TRUE(in.svc->store().add_draft_attachment("new", "a.txt", "unique-zebra-word"));
    in.env.model.push_text("ok");
    auto r = in.svc->send("new", "Look at the unique-zebra-word file", MessageMode::queue);
    ASSERT_TRUE(r);
    ASSERT_TRUE(in.env.wait(r->run_id, RunState::succeeded));
    in.svc->wait_idle();
    ASSERT_FALSE(in.svc->store().search("zebra").empty());
    ASSERT_TRUE(in.svc->store().save_draft_text(r->thread_id, "unsent"));

    ASSERT_TRUE(forget.forget("thread", r->thread_id));
    EXPECT_FALSE(in.svc->store().entry(r->thread_id));
    EXPECT_TRUE(in.svc->store().search("zebra").empty());
    EXPECT_TRUE(in.env.rt->runs(r->thread_id).empty());
    EXPECT_TRUE(in.svc->store().draft(r->thread_id).text.empty());
    auto st = in.env.db->prepare("SELECT COUNT(*) FROM thread_attachments WHERE thread_id=?1");
    st.bind(1, r->thread_id);
    ASSERT_TRUE(st.step());
    EXPECT_EQ(st.col_int(0), 0);
}

TEST(InboxDelete, RunningThreadMustBeStoppedFirstInterruptedOneIsCancelled) {
    Inbox in;
    in.env.start();
    const auto t = in.env.thread();
    Gate gate;
    in.env.model.push_blocking(gate);
    const auto r = in.env.run(t, "long");
    ASSERT_TRUE(in.env.wait(r, RunState::running));
    EXPECT_EQ(in.svc->delete_thread(t).error().code, "thread_busy");
    gate.release();
    ASSERT_TRUE(in.env.wait(r, RunState::succeeded));

    in.env.model.push_calls({{"q", "ask_user", {{"question", "Which one?"}}}});
    const auto r2 = in.env.run(t, "ask me");
    ASSERT_TRUE(in.env.wait(r2, RunState::interrupted));
    ASSERT_TRUE(in.svc->delete_thread(t));
    EXPECT_FALSE(in.svc->store().entry(t));
    EXPECT_TRUE(in.env.rt->pending_actions().empty());
}

// ==== 6.1 Toast content ==========================================================================================================

TEST(InboxToast, XmlCarriesTheThreadAndEscapesText) {
    const std::wstring x = toast_xml({"Q3 <numbers> & \"more\"", "Approval required: mail.send", "thr_abc"});
    EXPECT_NE(x.find(L"launch=\"thread=thr_abc\""), std::wstring::npos);
    EXPECT_NE(x.find(L"Q3 &lt;numbers&gt; &amp; &quot;more&quot;"), std::wstring::npos);
    EXPECT_EQ(x.find(L"<numbers>"), std::wstring::npos);
}

// ==== 5.4 Fault injection: the error event of each model-access code, retry, compact; the feed snapshot =============================

namespace {
json call(RpcRegistry& rpc, const std::string& method, const json& params) {
    const auto* h = rpc.find(method, false);
    EXPECT_NE(h, nullptr) << method;
    if (!h) return nullptr;
    auto r = (*h)(params);
    EXPECT_TRUE(r) << method << ": " << (r ? "" : r.error().code + " " + r.error().message);
    return r ? *r : json();
}
}  // namespace

// Folder changes made through the RPCs reach every window (improve-usability acceptance: a folder made elsewhere was
// missing from the navigation).
TEST(InboxFolders, EveryFolderChangeIsAnnounced) {
    Inbox in;
    RpcRegistry rpc;
    in.svc->register_rpc(rpc, false);
    const auto folder_events = [&] {
        const std::lock_guard lock(in.mx);
        return std::count_if(in.emitted.begin(), in.emitted.end(),
                             [](const auto& e) { return e.first == "inbox.changed" && jstr(e.second, "thread_id").empty(); });
    };
    const std::string id = jstr(call(rpc, "folders.create", {{"name", "Brakes"}}), "id");
    EXPECT_EQ(folder_events(), 1);
    call(rpc, "folders.rename", {{"id", id}, {"name", "Brake calibration"}});
    EXPECT_EQ(folder_events(), 2);
    call(rpc, "folders.delete", {{"id", id}});
    EXPECT_EQ(folder_events(), 3);
}

TEST(InboxErrors, EachModelAccessCodeReachesTheFeedAndRetryStartsANewRun) {
    Inbox in;
    in.env.start();
    RpcRegistry rpc;
    in.svc->register_rpc(rpc, false);
    for (const char* code : {model::code::auth_expired, model::code::rate_limit, model::code::model_unavailable, model::code::context_length,
                             model::code::timeout, model::code::quota_exceeded}) {
        const auto t = in.env.thread();
        model::ModelFailure f = failure(code);
        if (std::string(code) == model::code::quota_exceeded) f.reset_at = "2026-10-24T00:00:00Z";
        if (std::string(code) == model::code::rate_limit) f.retry_after_s = 30;
        // A fresh Thread has nothing to compact, so CONTEXT_LENGTH fails the Run at once as well.
        in.env.model.push_failure(f);
        const auto r = in.env.run(t, std::string("request for ") + code);
        ASSERT_TRUE(in.env.wait(r, RunState::failed)) << code;
        const json feed = call(rpc, "threads.feed", {{"thread_id", t}});
        ASSERT_EQ(feed["runs"].size(), 1u);
        json error = nullptr;
        for (const auto& e : feed["runs"][0]["events"])
            if (e["kind"] == "error") error = e["payload"];
        ASSERT_TRUE(error.is_object()) << code;
        EXPECT_EQ(error["code"], code);
        if (std::string(code) == model::code::quota_exceeded) EXPECT_EQ(error["reset_at"], "2026-10-24T00:00:00Z");
        if (std::string(code) == model::code::rate_limit) EXPECT_EQ(error["retry_after_s"], 30);
        EXPECT_EQ(feed["runs"][0]["state"], "failed");
    }
    // Retry sends the failed request again as a new Run.
    const auto t = in.env.thread();
    in.env.model.push_failure(failure(model::code::model_unavailable));
    const auto r = in.env.run(t, "please summarise");
    ASSERT_TRUE(in.env.wait(r, RunState::failed));
    in.env.model.push_text("summary");
    const json retried = call(rpc, "threads.retry", {{"thread_id", t}});
    ASSERT_TRUE(retried.contains("run_id"));
    ASSERT_TRUE(in.env.wait(retried["run_id"].get<std::string>(), RunState::succeeded));
    const auto reqs = in.env.model.requests();
    EXPECT_EQ(reqs.back().messages.back().content, "please summarise");
}

TEST(InboxFeed, SnapshotHoldsRunsEventsActionsAndQueuedMessagesSubagentsSeparately) {
    Inbox in;
    in.env.add_tool(mail_tool(in.env));
    in.env.policy->approvals.insert("mail.send");
    in.env.start();
    RpcRegistry rpc;
    in.svc->register_rpc(rpc, false);
    const auto t = in.env.thread();
    in.env.model.push_calls({{"c1", "find_tools", {{"query", "mail send"}}}});
    in.env.model.push_calls({{"c2", "mail.send", {{"to", "a@b.cz"}, {"rationale", "the user asked"}}}});
    const auto r = in.env.run(t, "send it");
    ASSERT_TRUE(in.env.wait(r, RunState::interrupted));
    const json feed = call(rpc, "threads.feed", {{"thread_id", t}});
    EXPECT_EQ(feed["entry"]["id"], t);
    ASSERT_EQ(feed["actions"].size(), 1u);
    EXPECT_EQ(feed["actions"][0]["payload"]["rationale"], "the user asked");
    std::set<std::string> kinds;
    for (const auto& e : feed["runs"][0]["events"]) kinds.insert(e["kind"].get<std::string>());
    for (const char* k : {"state", "message", "tool_call", "tool_result", "interrupt"}) EXPECT_TRUE(kinds.count(k)) << k;
    EXPECT_FALSE(kinds.count("nonce"));
    EXPECT_TRUE(feed["subagents"].empty());
    const json transcript = call(rpc, "runs.transcript", {{"run_id", r}});
    EXPECT_EQ(transcript["state"], "interrupted");
}
