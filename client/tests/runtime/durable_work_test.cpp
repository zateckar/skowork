// Effect-aware recovery in the agent runtime (improve-proactive-durable-work design D3, tasks 3.1-3.3 and 3.5): the
// operation ledger, unknown outcomes, the Unknown Effect Guard, recovery Questions, continuation and reconciliation.
#include "support/runtime_harness.h"

namespace {

constexpr const char* kContinue = "Continue without knowing";
constexpr const char* kStop = "Stop";

// A Run left running by a process that stopped while `calls` were dispatched: the user message, the assistant batch,
// the decisions and a ledger row per call, left dispatched unless `completed` holds its recorded result.
struct Crashed {
    std::string thread, run;
    std::map<std::string, std::string> ops;  // call id -> op id
};

Crashed crashed_run(Env& env, const std::vector<CallSpec>& calls, const std::map<std::string, std::string>& completed = {},
                    std::string thread = {}) {
    auto& store = env.rt->store();
    Crashed c;
    c.thread = thread.empty() ? env.thread() : thread;
    auto run = store.create_run(c.thread, {}, false);
    EXPECT_TRUE(run);
    c.run = run->id;
    EXPECT_TRUE(store.transition(c.run, RunState::running));
    EXPECT_TRUE(store.append_event(c.run, "message", user_message_event("do it")));
    model::ChatMessage assistant;
    assistant.role = "assistant";
    for (const auto& s : calls) assistant.tool_calls.push_back({s.id, s.tool, s.args.dump()});
    EXPECT_TRUE(store.append_event(c.run, "message", message_to_event(assistant)));
    for (const auto& s : calls) {
        EXPECT_TRUE(store.append_event(c.run, "tool_call", {{"call_id", s.id}, {"tool", s.tool}, {"args", s.args}, {"decision", "allow"}}));
        auto op = store.begin_operation(c.run, c.thread, s.id, s.tool, s.args, "unknown");
        EXPECT_TRUE(op);
        c.ops[s.id] = *op;
        if (const auto it = completed.find(s.id); it != completed.end())
            EXPECT_TRUE(store.finish_operation(*op, true, it->second, {{"source_kind", "tool"}, {"trusted", true}}, false));
    }
    return c;
}

std::vector<ActionItem> recovery_questions(Env& env, const std::string& thread) {
    std::vector<ActionItem> out;
    for (auto& a : env.rt->pending_actions(thread))
        if (jstr(a.payload, "raised_by") == "runtime.recovery") out.push_back(std::move(a));
    return out;
}

// The content of the tool message for `call_id` in the last model request.
std::string model_saw(Env& env, const std::string& call_id) {
    const auto reqs = env.model.requests();
    if (reqs.empty()) return "<no request>";
    for (const auto& m : reqs.back().messages)
        if (m.role == "tool" && m.tool_call_id == call_id) return m.content;
    return "<no tool message>";
}

ToolDef effect_tool(Env& env, const std::string& id = "mail.send") {
    ToolDef t = make_tool(env, id, Risk::external);
    t.core = true;
    return t;
}

// ---- 3.1 Operation ledger ---------------------------------------------------------------------------------------------

TEST(OperationLedger, ACallThatCannotBeRecordedIsNotMade) {
    Env env;
    env.add_tool(effect_tool(env));
    ASSERT_TRUE(env.db->exec("CREATE TRIGGER fail_ledger BEFORE INSERT ON tool_operations BEGIN SELECT RAISE(ABORT,'disk full'); END;"));
    env.model.push_calls({{"c1", "mail.send", json::object()}});
    env.model.push_text("done");
    env.start();
    const auto run = env.run(env.thread(), "send it");
    ASSERT_TRUE(env.wait(run, RunState::succeeded));
    EXPECT_EQ(env.calls.count("mail.send"), 0);
    EXPECT_NE(env.tool_content(run, "c1").find("not executed"), std::string::npos);
}

TEST(OperationLedger, EveryDispatchHasARowWithItsResultAndOperationId) {
    Env env;
    std::string seen_op;
    ToolDef t = effect_tool(env);
    t.run = [&](ToolContext& ctx, const json&) {
        seen_op = ctx.operation_id;
        return ToolOutput::success("sent #7");
    };
    env.add_tool(t);
    env.model.push_calls({{"c1", "mail.send", {{"to", "a"}}}});
    env.model.push_text("done");
    env.start();
    const auto run = env.run(env.thread(), "send it");
    ASSERT_TRUE(env.wait(run, RunState::succeeded));
    const auto ops = env.rt->store().operations_of_run(run);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].id, seen_op);
    EXPECT_EQ(ops[0].state, "completed");
    EXPECT_EQ(ops[0].content, "sent #7");
    EXPECT_EQ(ops[0].args, json({{"to", "a"}}));
    EXPECT_EQ(ops[0].recovery, "unknown");
}

TEST(OperationLedger, ReplaySafeRuntimeToolsAreMarkedSo) {
    Env env;
    ToolDef lazy = make_tool(env, "notes.lookup", Risk::read, "Look up a note");
    env.add_tool(lazy);
    env.model.push_calls({{"c1", "find_tools", {{"query", "note lookup"}}}});
    env.model.push_text("done");
    env.start();
    const auto run = env.run(env.thread(), "find it");
    ASSERT_TRUE(env.wait(run, RunState::succeeded));
    const auto ops = env.rt->store().operations_of_run(run);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].recovery, "replay_safe");
}

// ---- 3.3 Live unknown outcomes ----------------------------------------------------------------------------------------

TEST(UnknownOutcome, AToolExceptionAsksAndStopEndsTheRun) {
    Env env;
    ToolDef t = effect_tool(env);
    t.run = [](ToolContext&, const json&) -> ToolOutput { throw std::runtime_error("socket reset"); };
    env.add_tool(t);
    env.model.push_calls({{"c1", "mail.send", json::object()}});
    env.start();
    const auto thread = env.thread();
    const auto run = env.run(thread, "send it");
    ASSERT_TRUE(env.wait(run, RunState::interrupted));
    const auto pending = env.rt->pending_actions(thread);
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_TRUE(jbool(pending[0].payload, "unknown_outcome"));
    EXPECT_EQ(pending[0].payload["choices"], json({kContinue, kStop}));
    const auto ops = env.rt->store().operations_of_run(run);
    ASSERT_EQ(ops.size(), 1u);
    EXPECT_EQ(ops[0].state, "unknown");
    EXPECT_EQ(ops[0].action_id, pending[0].id);
    ASSERT_TRUE(env.rt->answer_question(pending[0].id, kStop));
    ASSERT_TRUE(env.wait(run, RunState::cancelled));
    EXPECT_EQ(env.rec(run).reason, "stopped by user: a call's outcome was unknown");
    EXPECT_EQ(env.rt->store().operation(ops[0].id)->resolution, "stop");
    EXPECT_EQ(env.model.request_count(), 1u);  // no model call after the unknown outcome
}

TEST(UnknownOutcome, ContinueTellsTheModelAndGuardsTheNextCall) {
    Env env;
    int sends = 0;
    ToolDef t = effect_tool(env);
    t.run = [&](ToolContext&, const json&) {
        ++sends;
        return sends == 1 ? ToolOutput::unknown("The request timed out after it was sent.") : ToolOutput::success("sent");
    };
    env.add_tool(t);
    env.model.push_calls({{"c1", "mail.send", json::object()}});
    env.model.push_calls({{"c2", "mail.send", json::object()}});  // the model tries again
    env.model.push_text("done");
    env.start();
    const auto thread = env.thread();
    const auto run = env.run(thread, "send it");
    ASSERT_TRUE(env.wait(run, RunState::interrupted));
    auto pending = env.rt->pending_actions(thread);
    ASSERT_EQ(pending.size(), 1u);
    ASSERT_TRUE(env.rt->answer_question(pending[0].id, kContinue));
    EXPECT_FALSE(env.rt->answer_question(pending[0].id, kContinue));  // answered once
    ASSERT_TRUE(env.wait_for([&] { return env.rt->pending_actions(thread).size() == 1u && env.rt->pending_actions(thread)[0].kind == "approval"; }));
    EXPECT_NE(model_saw(env, "c1").find("outcome of this call is unknown"), std::string::npos);
    pending = env.rt->pending_actions(thread);
    EXPECT_TRUE(jbool(pending[0].payload, "unknown_effect_guard"));
    EXPECT_EQ(sends, 1);  // the policy allowed it, but the guard holds it for the user
    ASSERT_TRUE(env.rt->resolve_approval(pending[0].id, ApprovalResolution::approve()));
    ASSERT_TRUE(env.wait(run, RunState::succeeded));
    EXPECT_EQ(sends, 2);
    EXPECT_EQ(env.rt->store().operations_of_run(run)[0].resolution, "continue");
}

TEST(UnknownEffectGuard, APolicyApprovalStaysOneApprovalAndOtherThreadsAreFree) {
    Env env;
    env.add_tool(effect_tool(env));
    env.policy->approvals.insert("mail.send");
    env.model.push_calls({{"c1", "mail.send", {{"rationale", "asked"}}}});
    env.model.push_text("done");
    env.model.push_calls({{"c2", "notes.write", json::object()}});
    env.model.push_text("done too");
    ToolDef notes = make_tool(env, "notes.write", Risk::read);
    notes.core = true;
    env.add_tool(notes);
    env.start();
    const auto guarded = env.thread();
    const auto crashed = crashed_run(env, {}, {}, guarded);
    const auto op = env.rt->store().begin_operation(crashed.run, guarded, "x", "mail.send", json::object(), "unknown");
    ASSERT_TRUE(op);
    ASSERT_TRUE(env.rt->store().finish_operation(*op, false, "", json::object(), true));
    ASSERT_TRUE(env.rt->store().transition(crashed.run, RunState::failed, "test"));
    const auto run = env.run(guarded, "send it");
    ASSERT_TRUE(env.wait(run, RunState::interrupted));
    const auto pending = env.rt->pending_actions(guarded);
    ASSERT_EQ(pending.size(), 1u);  // one Approval satisfies the policy and the guard
    EXPECT_FALSE(jbool(pending[0].payload, "unknown_effect_guard"));
    ASSERT_TRUE(env.rt->resolve_approval(pending[0].id, ApprovalResolution::approve()));
    ASSERT_TRUE(env.wait(run, RunState::succeeded));
    EXPECT_EQ(env.calls.count("mail.send"), 1);
    // A new Thread starts without the guard.
    const auto free = env.run(env.thread(), "write a note");
    ASSERT_TRUE(env.wait(free, RunState::succeeded));
    EXPECT_EQ(env.calls.count("notes.write"), 1);
}

TEST(UnknownEffectGuard, ChildRunCallsAreGuardedToo) {
    Env env;
    env.add_tool(effect_tool(env));
    env.model.set_responder([](const model::ChatRequest& req) -> std::optional<model::ModelResult<model::ChatResponse>> {
        if (req.messages.empty() || req.messages[0].content.rfind("You are a subagent", 0) != 0) return std::nullopt;
        bool answered = false;
        for (const auto& m : req.messages) answered = answered || m.role == "tool";
        if (answered) return model::ModelResult<model::ChatResponse>(text_response("child done"));
        return model::ModelResult<model::ChatResponse>(calls_response({{"cc", "mail.send", json::object()}}));
    });
    env.model.push_calls({{"d1", "delegate", {{"task", "send it"}, {"tools", json::array({"mail.send"})}}}});
    env.model.push_text("parent done");
    env.start();
    const auto thread = env.thread();
    const auto earlier = crashed_run(env, {}, {}, thread);
    const auto op = env.rt->store().begin_operation(earlier.run, thread, "x", "mail.send", json::object(), "unknown");
    ASSERT_TRUE(op && env.rt->store().finish_operation(*op, false, "", json::object(), true));
    ASSERT_TRUE(env.rt->store().transition(earlier.run, RunState::failed, "test"));
    const auto run = env.run(thread, "delegate");
    ASSERT_TRUE(env.wait_for([&] {
        for (const auto& a : env.rt->pending_actions(thread))
            if (a.kind == "approval" && jbool(a.payload, "unknown_effect_guard")) return true;
        return false;
    }));
    EXPECT_EQ(env.calls.count("mail.send"), 0);
    ASSERT_TRUE(env.rt->cancel(run));
    ASSERT_TRUE(env.wait(run, RunState::cancelled));
}

// ---- 3.2/3.3 Shutdown recovery and continuation ---------------------------------------------------------------------

TEST(ShutdownRecovery, AnUnknownCallAsksThenContinuesOnceWithoutRepeatingIt) {
    Env env;
    env.add_tool(effect_tool(env));
    const auto c = crashed_run(env, {{"c1", "mail.send", {{"to", "a"}}}});
    ASSERT_TRUE(env.db->exec("UPDATE runs SET model_calls=3, tool_calls=1 WHERE id='" + c.run + "'"));
    env.model.push_text("I could not confirm the send.");
    env.start();
    EXPECT_EQ(env.report.unknown_roots, std::vector<std::string>{c.run});
    EXPECT_EQ(env.rt->store().operation(c.ops.at("c1"))->state, "unknown");
    auto qs = recovery_questions(env, c.thread);
    ASSERT_EQ(qs.size(), 1u);
    EXPECT_EQ(jstr(qs[0].payload, "op_id"), c.ops.at("c1"));
    EXPECT_TRUE(qs[0].run_id.empty());  // standalone: no Run is running

    auto early = env.rt->continue_run(c.run);
    ASSERT_FALSE(early);
    EXPECT_EQ(early.error().code, "not_continuable");
    EXPECT_TRUE(jbool(env.rt->recovery_status(c.run), "pending"));

    ASSERT_TRUE(env.rt->answer_question(qs[0].id, kContinue));
    EXPECT_FALSE(env.rt->answer_question(qs[0].id, kContinue));
    EXPECT_TRUE(jbool(env.rt->recovery_status(c.run), "continuable"));

    auto cont = env.rt->retry_run(c.thread);  // Run Retry of a shutdown-failed Run starts the continuation
    ASSERT_TRUE(cont) << cont.error().message;
    EXPECT_NE(*cont, c.run);
    ASSERT_TRUE(env.wait(*cont, RunState::succeeded));
    EXPECT_EQ(env.calls.count("mail.send"), 0);
    EXPECT_NE(model_saw(env, "c1").find("outcome of this call is unknown"), std::string::npos);
    const auto failed = env.rec(c.run);
    EXPECT_EQ(failed.state, RunState::failed);  // the failed Run and its history stay as they were
    EXPECT_EQ(env.rec(*cont).continues_run_id, c.run);
    EXPECT_GE(env.rec(*cont).model_calls, 4);  // carried budget plus its own call
    auto twice = env.rt->continue_run(c.run);
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().code, "not_continuable");
    EXPECT_EQ(jstr(env.rt->recovery_status(c.run), "continuation"), *cont);
    // Continuation Blocker Codes (improve-plain-information 1.1): a code for the window, the old text for errors.
    EXPECT_EQ(jstr(env.rt->recovery_status(c.run), "blocker_code"), "already_continued");
    EXPECT_EQ(jstr(env.rt->recovery_status(c.run), "blocker"), "the Run was already continued");
}

TEST(ShutdownRecovery, OneStopEndsTheFailedRunForGood) {
    Env env;
    env.add_tool(effect_tool(env));
    env.add_tool(effect_tool(env, "files.delete"));
    const auto c = crashed_run(env, {{"c1", "mail.send", json::object()}, {"c2", "files.delete", json::object()}});
    env.start();
    auto qs = recovery_questions(env, c.thread);
    ASSERT_EQ(qs.size(), 2u);
    ASSERT_TRUE(env.rt->answer_question(qs[0].id, kStop));
    EXPECT_TRUE(recovery_questions(env, c.thread).empty());  // the other Question is withdrawn
    const auto status = env.rt->recovery_status(c.run);
    EXPECT_TRUE(jbool(status, "stopped"));
    EXPECT_FALSE(jbool(status, "continuable"));
    EXPECT_EQ(jstr(status, "blocker_code"), "stopped");
    auto r = env.rt->retry_run(c.thread);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, "not_continuable");
    env.restart();
    EXPECT_TRUE(recovery_questions(env, c.thread).empty());
    EXPECT_FALSE(env.rt->continue_run(c.run));
}

TEST(ShutdownRecovery, ARecordedResultIsReusedAndNothingIsAsked) {
    Env env;
    env.add_tool(effect_tool(env));
    const auto c = crashed_run(env, {{"c1", "mail.send", json::object()}}, {{"c1", "sent as #42"}});
    env.model.push_text("It was sent.");
    env.start();
    EXPECT_TRUE(env.report.unknown_roots.empty());
    EXPECT_TRUE(recovery_questions(env, c.thread).empty());
    auto cont = env.rt->continue_run(c.run);
    ASSERT_TRUE(cont) << cont.error().message;
    ASSERT_TRUE(env.wait(*cont, RunState::succeeded));
    EXPECT_EQ(model_saw(env, "c1"), "sent as #42");
    EXPECT_EQ(env.calls.count("mail.send"), 0);
}

TEST(ShutdownRecovery, QuestionsSurviveARestartWithoutDuplicates) {
    Env env;
    env.add_tool(effect_tool(env));
    const auto c = crashed_run(env, {{"c1", "mail.send", json::object()}});
    env.start();
    ASSERT_EQ(recovery_questions(env, c.thread).size(), 1u);
    env.restart();
    env.restart();
    const auto qs = recovery_questions(env, c.thread);
    ASSERT_EQ(qs.size(), 1u);
    ASSERT_TRUE(env.rt->answer_question(qs[0].id, kContinue));
    EXPECT_TRUE(jbool(env.rt->recovery_status(c.run), "continuable"));
}

TEST(ShutdownRecovery, ALegacyRunWithoutLedgerTreatsDecidedCallsAsUnknown) {
    Env env;
    env.add_tool(effect_tool(env));
    auto& store = env.rt->store();
    const auto thread = env.thread();
    auto run = store.create_run(thread, {}, false);
    ASSERT_TRUE(run);
    ASSERT_TRUE(store.append_event(run->id, "message", user_message_event("send it")));
    model::ChatMessage assistant;
    assistant.role = "assistant";
    assistant.tool_calls.push_back({"c1", "mail.send", "{}"});
    assistant.tool_calls.push_back({"c2", "mail.send", "{}"});
    ASSERT_TRUE(store.append_event(run->id, "message", message_to_event(assistant)));
    ASSERT_TRUE(store.append_event(run->id, "tool_call", {{"call_id", "c1"}, {"tool", "mail.send"}, {"decision", "allow"}}));
    ASSERT_TRUE(store.append_event(run->id, "tool_call", {{"call_id", "c2"}, {"tool", "mail.send"}, {"decision", "deny"}}));
    ASSERT_TRUE(store.transition(run->id, RunState::running));
    ASSERT_TRUE(store.transition(run->id, RunState::failed, "interrupted by shutdown"));
    ASSERT_TRUE(env.db->exec("UPDATE runs SET ledger=0 WHERE id='" + run->id + "'"));  // written before the ledger existed
    env.start();
    const auto ops = store.operations_of_run(run->id);
    ASSERT_EQ(ops.size(), 1u);  // the allowed call; the denied one was never made
    EXPECT_EQ(ops[0].call_id, "c1");
    EXPECT_EQ(ops[0].state, "unknown");
    EXPECT_EQ(recovery_questions(env, thread).size(), 1u);
    EXPECT_TRUE(env.rt->store().thread_has_unknown(thread));
}

TEST(ShutdownRecovery, AnInterruptedApprovalIsNotADispatch) {
    Env env;
    env.add_tool(effect_tool(env));
    env.policy->approvals.insert("mail.send");
    env.model.push_calls({{"c1", "mail.send", {{"rationale", "asked"}}}});
    env.start();
    const auto thread = env.thread();
    const auto run = env.run(thread, "send it");
    ASSERT_TRUE(env.wait(run, RunState::interrupted));
    env.restart();  // the Run waits for the user; nothing was dispatched
    EXPECT_TRUE(env.rt->store().operations_of_run(run).empty());
    EXPECT_TRUE(recovery_questions(env, thread).empty());
    EXPECT_FALSE(env.rt->store().thread_has_unknown(thread));
}

// ---- 3.5 Reconciliation (controlled provider fake) ------------------------------------------------------------------

TEST(Reconciliation, AProviderFakeSettlesOrLeavesUnknown) {
    struct Case {
        std::function<Reconciliation(const std::string&, const json&)> fn;
        std::string state;
        size_t questions;
    };
    const std::vector<Case> cases = {
        {[](const std::string&, const json&) { return Reconciliation{Reconciliation::Kind::succeeded, "found message #42"}; }, "completed", 0},
        {[](const std::string&, const json&) { return Reconciliation{Reconciliation::Kind::absent, ""}; }, "abandoned", 0},
        {[](const std::string&, const json&) { return Reconciliation{Reconciliation::Kind::unknown, ""}; }, "unknown", 1},
        {[](const std::string&, const json&) -> Reconciliation { throw std::runtime_error("timed out"); }, "unknown", 1},
    };
    for (const auto& k : cases) {
        Env env;
        std::string asked_for;
        ToolDef t = effect_tool(env);
        t.recovery = Recovery::reconcilable;
        t.reconcile = [&, fn = k.fn](const std::string& op, const json& args) {
            asked_for = op;
            return fn(op, args);
        };
        env.add_tool(t);
        auto& store = env.rt->store();
        const auto c = crashed_run(env, {});
        auto op = store.begin_operation(c.run, c.thread, "c1", "mail.send", json::object(), "reconcilable");
        ASSERT_TRUE(op);
        env.start();
        EXPECT_EQ(asked_for, *op);
        EXPECT_EQ(store.operation(*op)->state, k.state);
        EXPECT_EQ(recovery_questions(env, c.thread).size(), k.questions);
        EXPECT_EQ(env.calls.count("mail.send"), 0);  // reconciliation never repeats the call
    }
}

TEST(Reconciliation, ALostCommitIsReconciledAgainAndTheStaleQuestionWithdrawn) {
    Env env;
    int checks = 0;
    ToolDef t = effect_tool(env);
    t.recovery = Recovery::reconcilable;
    t.reconcile = [&](const std::string&, const json&) {
        ++checks;
        return Reconciliation{Reconciliation::Kind::succeeded, "found"};
    };
    env.add_tool(t);
    auto& store = env.rt->store();
    const auto c = crashed_run(env, {});
    auto op = store.begin_operation(c.run, c.thread, "c1", "mail.send", json::object(), "reconcilable");
    ASSERT_TRUE(op);
    ASSERT_TRUE(env.db->exec("CREATE TRIGGER lose_commit BEFORE UPDATE ON tool_operations WHEN NEW.state='completed' "
                             "BEGIN SELECT RAISE(ABORT,'crash'); END;"));
    env.start();
    EXPECT_EQ(store.operation(*op)->state, "unknown");
    EXPECT_EQ(recovery_questions(env, c.thread).size(), 1u);
    ASSERT_TRUE(env.db->exec("DROP TRIGGER lose_commit;"));
    env.restart();
    EXPECT_EQ(checks, 2);
    EXPECT_EQ(env.rt->store().operation(*op)->state, "completed");
    EXPECT_TRUE(recovery_questions(env, c.thread).empty());
    EXPECT_TRUE(jbool(env.rt->recovery_status(c.run), "continuable"));
}

}  // namespace
