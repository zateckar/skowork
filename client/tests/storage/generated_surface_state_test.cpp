#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "platform/crypto.h"
#include "inbox/inbox_store.h"
#include "runtime/run_store.h"
#include "storage/app_store.h"
#include "storage/generated_surface_state.h"
#include "storage/generated_surface_submission.h"

using namespace wb;

namespace {
struct TempSurfaceDb {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("skowork-surface-test-" + random_urlsafe(8));
    db_key key{};
    TempSurfaceDb() { std::filesystem::create_directories(path); key[0] = 17; }
    ~TempSurfaceDb() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
const std::string kBlock = "```skowork-ui\n{\"version\":1,\"id\":\"s\",\"summary\":\"S\",\"components\":[]}\n```\n";
}  // namespace

TEST(GeneratedSurfaceState, ExistingSchemaKeepsDraftPrivateAndOldClientCanDeleteThread) {
    TempSurfaceDb temp;
    std::vector<Migration> older;
    for (const auto& migration : app_migrations()) if (migration.version < 1800) older.push_back(migration);
    MigrationOutcome initial;
    auto opened = open_and_migrate(temp.path / "app.db", temp.key, older, initial);
    ASSERT_TRUE(opened);
    ASSERT_TRUE((*opened)->exec("INSERT INTO threads(id,title,created_at,updated_at) VALUES('t','T','now','now');"
                                "INSERT INTO runs(id,thread_id,state,origin_kind,queue_seq,created_at) VALUES('r','t','succeeded','user_message',1,'now');"));
    {
        auto message = (*opened)->prepare("INSERT INTO run_events(run_id,seq,at,kind,payload) VALUES('r',1,'now','message',?1)");
        message.bind(1, json{{"role", "assistant"}, {"content", kBlock}, {"surface_complete", true}}.dump());
        ASSERT_TRUE(message.run());
    }
    const auto schema_version = (*opened)->user_version();
    GeneratedSurfaceStateStore states(**opened);
    auto binding = states.bind_assistant("t", "r", 1, 0);
    ASSERT_TRUE(binding);
    const json draft = {{"note", "never-show-private-draft"}};
    const auto saved = states.save(*binding, draft);
    ASSERT_TRUE(saved) << saved.error().message;
    EXPECT_EQ((*opened)->user_version(), schema_version);
    {
        auto record = (*opened)->prepare("SELECT id,content FROM thread_attachments WHERE thread_id='t'");
        ASSERT_TRUE(record.step());
        const auto id = record.col_text(0);
        const auto raw = record.col_blob(1);
        EXPECT_EQ(std::string(raw.begin(), raw.end()).find("never-show-private-draft"), std::string::npos);
        runtime::RunStore run_store(**opened, system_clock());
        EXPECT_FALSE(run_store.attachment(id));
        EXPECT_FALSE(run_store.read_attachment(id, 0, 4096));
        EXPECT_FALSE(record.step());
    }
    opened.value().reset();
    MigrationOutcome downgraded;
    opened = open_and_migrate(temp.path / "app.db", temp.key, older, downgraded);
    ASSERT_TRUE(opened);
    EXPECT_FALSE(downgraded.degraded);
    GeneratedSurfaceStateStore restored(**opened);
    const auto saved_again = restored.load(*binding);
    ASSERT_TRUE(saved_again);
    ASSERT_TRUE(saved_again->has_value());
    EXPECT_EQ(**saved_again, draft);
    inbox::InboxStore old_inbox(**opened, system_clock());
    ASSERT_TRUE(old_inbox.delete_thread("t"));
    auto remaining = (*opened)->prepare("SELECT COUNT(*) FROM thread_attachments");
    ASSERT_TRUE(remaining.step());
    EXPECT_EQ(remaining.col_int(0), 0);
}

TEST(GeneratedSurfaceState, QuestionDraftIsBoundToTheOpenQuestionAndRevision) {
    TempSurfaceDb temp;
    MigrationOutcome outcome;
    auto opened = open_and_migrate(temp.path / "app.db", temp.key, app_migrations(), outcome);
    ASSERT_TRUE(opened);
    ASSERT_TRUE((*opened)->exec("INSERT INTO threads(id,title,created_at,updated_at) VALUES('t','T','now','now');"
                                "INSERT INTO runs(id,thread_id,state,origin_kind,queue_seq,created_at) VALUES('r','t','interrupted','user_message',1,'now');"));
    const json form = json::parse(R"({"version":1,"id":"f","summary":"S","components":[{"type":"text","id":"note","label":"Note","value":""},{"type":"button","id":"send","label":"Send"}]})");
    const auto revision = hex_encode(sha256(form.dump()));
    runtime::RunStore runs(**opened, system_clock());
    const auto action = runs.create_action("r", "question", "q", {{"surface", form}, {"surface_revision", revision}, {"surface_token", "host-token"}});
    ASSERT_TRUE(action);
    GeneratedSurfaceStateStore states(**opened);
    const auto binding = states.bind_question("t", "r", action->id, revision, "host-token");
    ASSERT_TRUE(binding);
    const json draft = {{"draft", {{"note", "private question draft"}}}};
    ASSERT_TRUE(states.save_question(*binding, draft));
    EXPECT_FALSE(states.bind_question("other", "r", action->id, revision, "host-token"));
    EXPECT_FALSE(states.bind_question("t", "r", action->id, "stale", "host-token"));
    EXPECT_FALSE(states.bind_question("t", "r", action->id, revision, "forged"));
    opened.value().reset();
    opened = open_and_migrate(temp.path / "app.db", temp.key, app_migrations(), outcome);
    ASSERT_TRUE(opened);
    GeneratedSurfaceStateStore restarted(**opened);
    auto saved = restarted.load_question(*binding);
    ASSERT_TRUE(saved);
    ASSERT_TRUE(saved->has_value());
    EXPECT_EQ(**saved, draft);
    runtime::RunStore restarted_runs(**opened, system_clock());
    ASSERT_TRUE(restarted_runs.resolve_action(action->id, {{"kind", "answer"}, {"answer", "text"}}));
    EXPECT_FALSE(restarted.load_question(*binding));
    ASSERT_TRUE((*opened)->exec("UPDATE runs SET state='cancelled' WHERE id='r'"));
    inbox::InboxStore inbox(**opened, system_clock());
    ASSERT_TRUE(inbox.delete_thread("t"));
    auto remaining = (*opened)->prepare("SELECT COUNT(*) FROM thread_attachments");
    ASSERT_TRUE(remaining.step());
    EXPECT_EQ(remaining.col_int(0), 0);
}

TEST(GeneratedSurfaceState, UpgradeRevisionIsolationEncryptionAndThreadDeletion) {
    TempSurfaceDb temp;
    std::vector<Migration> old;
    for (const auto& migration : app_migrations()) if (migration.version < 1800) old.push_back(migration);
    MigrationOutcome before;
    auto previous = open_and_migrate(temp.path / "app.db", temp.key, old, before);
    ASSERT_TRUE(previous);
    ASSERT_TRUE((*previous)->exec("INSERT INTO threads(id,title,created_at,updated_at) VALUES('t','T','now','now');"
                                  "INSERT INTO runs(id,thread_id,state,origin_kind,queue_seq,created_at) VALUES('r','t','succeeded','user_message',1,'now');"));
    {
        auto message = (*previous)->prepare("INSERT INTO run_events(run_id,seq,at,kind,payload) VALUES('r',1,'now','message',?1)");
        message.bind(1, json{{"role", "assistant"}, {"content", kBlock}, {"surface_complete", true}}.dump());
        ASSERT_TRUE(message.run());
    }
    previous.value().reset();
    MigrationOutcome upgraded;
    auto opened = open_and_migrate(temp.path / "app.db", temp.key, app_migrations(), upgraded);
    ASSERT_TRUE(opened);
    ASSERT_FALSE(upgraded.degraded) << upgraded.failure;
    EXPECT_FALSE(upgraded.migrated);  // the versioned record format requires no database schema change
    GeneratedSurfaceStateStore store(**opened);
    auto binding = store.bind_assistant("t", "r", 1, 0);
    ASSERT_TRUE(binding);
    const json draft = {{"note", "private-surface-draft-123"}};
    ASSERT_TRUE(store.save(*binding, draft));
    auto loaded = store.load(*binding);
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(loaded->has_value());
    EXPECT_EQ(**loaded, draft);
    EXPECT_FALSE(store.bind_assistant("other", "r", 1, 0));
    EXPECT_FALSE(store.bind_assistant("t", "r", 1, 1));
    opened.value().reset();
    MigrationOutcome restarted;
    opened = open_and_migrate(temp.path / "app.db", temp.key, app_migrations(), restarted);
    ASSERT_TRUE(opened);
    GeneratedSurfaceStateStore resumed(**opened);
    auto after_restart = resumed.load(*binding);
    ASSERT_TRUE(after_restart);
    ASSERT_TRUE(after_restart->has_value());
    EXPECT_EQ(**after_restart, draft);
    {
        auto replace = (*opened)->prepare("UPDATE run_events SET payload=?1 WHERE run_id='r' AND seq=1");
        replace.bind(1, json{{"role", "assistant"}, {"content", kBlock + "\nChanged"}, {"surface_complete", false}}.dump());
        ASSERT_TRUE(replace.run());
        EXPECT_FALSE(resumed.load(*binding));
        EXPECT_FALSE(resumed.save(*binding, draft));
        replace.reset();
        replace.bind(1, json{{"role", "assistant"}, {"content", "```skowork-ui\n{\"version\":1,\"id\":\"s\",\"summary\":\"New\",\"components\":[]}\n```"}, {"surface_complete", true}}.dump());
        ASSERT_TRUE(replace.run());
    }
    auto newer = resumed.bind_assistant("t", "r", 1, 0);
    ASSERT_TRUE(newer);
    EXPECT_NE(newer->revision, binding->revision);
    auto empty = resumed.load(*newer);
    ASSERT_TRUE(empty);
    EXPECT_FALSE(empty->has_value());
    ASSERT_TRUE(resumed.save(*newer, draft));
    inbox::InboxStore inbox(**opened, system_clock());
    ASSERT_TRUE(inbox.delete_thread("t"));
    {
        auto count = (*opened)->prepare("SELECT COUNT(*) FROM thread_attachments");
        ASSERT_TRUE(count.step());
        EXPECT_EQ(count.col_int(0), 0);
    }
    opened.value().reset();
    std::ifstream bytes(temp.path / "app.db", std::ios::binary);
    std::stringstream raw;
    raw << bytes.rdbuf();
    EXPECT_EQ(raw.str().find("private-surface-draft-123"), std::string::npos);
}

TEST(GeneratedSurfaceSubmission, ValidatesAndEnqueuesAtomicallyAndRetriesIdempotently) {
    TempSurfaceDb temp;
    std::vector<Migration> older;
    for (const auto& migration : app_migrations()) if (migration.version < 1800) older.push_back(migration);
    MigrationOutcome outcome;
    auto opened = open_and_migrate(temp.path / "app.db", temp.key, older, outcome);
    ASSERT_TRUE(opened);
    ASSERT_TRUE((*opened)->exec("INSERT INTO threads(id,title,created_at,updated_at) VALUES('t','T','now','now');"
                                  "INSERT INTO runs(id,thread_id,state,origin_kind,queue_seq,created_at) VALUES('r','t','succeeded','user_message',1,'now');"));
    const std::string text = R"(```skowork-ui
{"version":1,"id":"s","summary":"S","components":[{"type":"section","id":"choices","label":"Choices","components":[{"type":"select","id":"cadence","label":"Cadence","options":["Weekly","Monthly"]}]},{"type":"button","id":"send","label":"Continue"},{"type":"button","id":"other","label":"Other"}]}
```
)";
    {
        auto message = (*opened)->prepare("INSERT INTO run_events(run_id,seq,at,kind,payload) VALUES('r',1,'now','message',?1)");
        message.bind(1, json{{"role", "assistant"}, {"content", text}, {"surface_complete", true}}.dump());
        ASSERT_TRUE(message.run());
    }
    GeneratedSurfaceStateStore states(**opened);
    const auto binding = states.bind_assistant("t", "r", 1, 0);
    ASSERT_TRUE(binding);
    GeneratedSurfaceSubmissionStore submissions(**opened);
    auto second_connection = Database::open(temp.path / "app.db", temp.key);
    ASSERT_TRUE(second_connection);
    GeneratedSurfaceSubmissionStore other(**second_connection);
    std::vector<std::string> tokens(8);
    std::vector<std::thread> preparers;
    for (size_t i = 0; i < tokens.size(); ++i) preparers.emplace_back([&, i] {
        auto& store = i % 2 ? other : submissions;
        const auto result = store.prepare(*binding, "other");
        if (result) tokens[i] = *result;
    });
    for (auto& worker : preparers) worker.join();
    ASSERT_FALSE(tokens.front().empty());
    for (const auto& candidate : tokens) EXPECT_EQ(candidate, tokens.front());
    EXPECT_FALSE(submissions.prepare(*binding, "cadence"));
    const auto ticket = submissions.prepare(*binding, "send");
    ASSERT_TRUE(ticket);
    const auto before_acceptance = submissions.accepted(*binding);
    ASSERT_TRUE(before_acceptance);
    EXPECT_FALSE(*before_acceptance);
    const auto same_ticket = submissions.prepare(*binding, "send");
    ASSERT_TRUE(same_ticket);
    EXPECT_EQ(*ticket, *same_ticket);
    auto forged_ticket = *ticket;
    forged_ticket.back() = forged_ticket.back() == 'A' ? 'B' : 'A';
    EXPECT_FALSE(submissions.submit(forged_ticket, {{"cadence", "Weekly"}}, "queue"));
    EXPECT_FALSE(submissions.submit(*ticket, {{"cadence", "Weekly"}}, "queue", false));
    EXPECT_FALSE(submissions.submit(*ticket, {{"cadence", "Invalid"}}, "queue"));
    EXPECT_FALSE(submissions.submit(*ticket, {{"cadence", "Weekly"}, {"hidden", true}}, "queue"));
    EXPECT_FALSE(submissions.submit(*ticket, {{"cadence", "Weekly"}}, "queue", true, {{"choices", false}}));
    ASSERT_TRUE((*opened)->exec("CREATE TRIGGER fail_surface_queue BEFORE INSERT ON queued_messages BEGIN SELECT RAISE(ABORT,'test failure'); END;"));
    EXPECT_FALSE(submissions.submit(*ticket, {{"cadence", "Weekly"}}, "queue"));
    ASSERT_TRUE((*opened)->exec("DROP TRIGGER fail_surface_queue"));
    ASSERT_TRUE((*opened)->exec("CREATE TRIGGER fail_surface_accept BEFORE UPDATE ON thread_attachments "
                                "WHEN OLD.call_id='generated_surface' BEGIN SELECT RAISE(ABORT,'test failure'); END;"));
    EXPECT_FALSE(submissions.submit(*ticket, {{"cadence", "Weekly"}}, "queue"));
    {
        auto empty = (*opened)->prepare("SELECT COUNT(*) FROM queued_messages");
        ASSERT_TRUE(empty.step());
        EXPECT_EQ(empty.col_int(0), 0);
    }
    ASSERT_TRUE((*opened)->exec("DROP TRIGGER fail_surface_accept"));
    std::vector<long long> outcomes(8, 0);
    std::vector<std::thread> workers;
    for (size_t i = 0; i < outcomes.size(); ++i) workers.emplace_back([&, i] {
        auto& store = i % 2 ? other : submissions;
        const auto result = store.submit(*ticket, {{"cadence", "Weekly"}}, "queue");
        if (result) outcomes[i] = *result;
    });
    for (auto& worker : workers) worker.join();
    ASSERT_GT(outcomes.front(), 0);
    for (const auto outcome_id : outcomes) EXPECT_EQ(outcome_id, outcomes.front());
    const auto surface_accepted = submissions.accepted(*binding);
    ASSERT_TRUE(surface_accepted);
    EXPECT_TRUE(*surface_accepted);
    EXPECT_FALSE(other.submit(tokens.front(), {{"cadence", "Weekly"}}, "queue"));
    const auto accepted = submissions.submit(*ticket, {{"cadence", "Weekly"}}, "queue");
    ASSERT_TRUE(accepted);
    auto replace_message = [&](const std::string& content) {
        auto update = (*opened)->prepare("UPDATE run_events SET payload=?1 WHERE run_id='r' AND seq=1");
        update.bind(1, json{{"role", "assistant"}, {"content", content}, {"surface_complete", true}}.dump());
        return update.run();
    };
    auto revised_text = text;
    revised_text.replace(revised_text.find("Continue"), std::string("Continue").size(), "Proceed");
    ASSERT_TRUE(replace_message(revised_text));
    const auto revised = states.bind_assistant("t", "r", 1, 0);
    ASSERT_TRUE(revised);
    ASSERT_NE(revised->revision, binding->revision);
    const auto revised_accepted = submissions.accepted(*revised);
    ASSERT_TRUE(revised_accepted);
    EXPECT_FALSE(*revised_accepted);
    ASSERT_TRUE(replace_message(text));
    second_connection.value().reset();
    opened.value().reset();
    MigrationOutcome restarted;
    opened = open_and_migrate(temp.path / "app.db", temp.key, older, restarted);
    ASSERT_TRUE(opened);
    GeneratedSurfaceSubmissionStore resumed(**opened);
    const auto accepted_after_restart = resumed.accepted(*binding);
    ASSERT_TRUE(accepted_after_restart);
    EXPECT_TRUE(*accepted_after_restart);
    EXPECT_FALSE(resumed.submit(tokens.front(), {{"cadence", "Weekly"}}, "queue"));
    const auto retry = resumed.submit(*ticket, {{"cadence", "Monthly"}}, "queue", false);
    ASSERT_TRUE(retry);
    EXPECT_EQ(*accepted, *retry);
    auto queued = (*opened)->prepare("SELECT text,mode FROM queued_messages WHERE id=?1");
    queued.bind(1, *accepted);
    ASSERT_TRUE(queued.step());
    EXPECT_NE(queued.col_text(0).find("Cadence"), std::string::npos);
    EXPECT_NE(queued.col_text(0).find("Weekly"), std::string::npos);
    EXPECT_EQ(queued.col_text(1), "queue");
    auto count = (*opened)->prepare("SELECT COUNT(*) FROM queued_messages");
    ASSERT_TRUE(count.step());
    EXPECT_EQ(count.col_int(0), 1);
}
