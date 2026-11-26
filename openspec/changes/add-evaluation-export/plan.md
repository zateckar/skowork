# Evaluation Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. When a task here finishes, also check the matching item in `tasks.md`, which is the OpenSpec checklist.

**Goal:** Measure triage, Agenda, knowledge, speaker naming and transcription on a developer's real data. The developer exports items, results and decisions through the local pipe, then replays one step with variant instructions. This change also adds a user decision log and content-free daily quality counters.

**Architecture:**
- A new `client/src/eval/` module owns:
  - the instruction registry;
  - the decision log;
  - the quality counters;
  - the exporter;
  - the replayer.
- Feature code reads its instructions and settings from the registry.
- Decision handlers write to the log.
- `eval.*` RPC methods are pipe-only and gated by `policy.evaluation.export`.
- Scoring lives outside the client, in `dev/eval/` (Node.js and TypeScript).

**Tech Stack:**
- Client: C++20, MinGW GCC, CMake, vendored GoogleTest, nlohmann::json, SQLite3MultipleCiphers, sherpa-onnx (speech worker).
- Dev tooling: Node 24 ESM TypeScript run directly, `node --test`, the same layout as `dev/graph-mock`.

**Spec:** `openspec/changes/add-evaluation-export/`:
- `proposal.md`, `design.md` and `tasks.md`;
- `specs/client/evaluation/spec.md`;
- `specs/client/telemetry/spec.md`.

## Global Constraints

- **Policy key** `evaluation.export`: values `off` | `on`. The default is `off`, and a missing key or an unknown value means off.
- **Exposure:** eval methods are offered on the pipe only, never to the window. They are never a `runtime::ToolDef`.
- **When the setting is off:** every eval method returns the error `not_enabled` and writes nothing.
- **Replay `max_items`:** default 200, at least 1, at most 2000. Only one replay runs at a time.
- **Decision log:** entries are kept 365 days and removed when their item is forgotten or deleted. Each `before` and `after` value is at most 2 KB, truncated as JSON string `"…"` when larger.
- **Instruction id:** the first 12 hex characters of the SHA-256 of the canonical JSON (`json::dump()` with sorted keys) of a step's effective entries.
- **Migrations:**
  - `app.db` uses the next free block, 2800–2899. Number 2800 is the decision log, 2801 the speaker metadata, 2802 the quality tables.
  - `kg.db` is untouched.
- **Live behaviour is unchanged:** the golden tests from Task 1 must pass byte for byte after every task.
- **No real data in the repo:** real exports go only under `dev/eval-data/`, which is git-ignored. Fixtures are synthetic.
- **Spec terms:** use them exactly as written (Thread, Run, Source, Model role, Agenda item, Teams chat).
- **Build and test commands:** `cd client; cmake --preset debug; cmake --build --preset debug --target <target>`, then `./build/debug/bin/<target>.exe --gtest_filter=<Suite.Name>`. The full suite is `ctest --preset debug -LE e2e`.

## Review Focus

1. **Hostile and odd item text.** Lone UTF-16 surrogates from Outlook COM, NUL characters, a 5 MB body or a subject made of newlines must give one valid JSON line per record and must never abort the export. Owner: Task 9 (fuzz and edge tests).
2. **An export folder that already exists, cannot be written, or fills the disk part-way.** The export must fail with a clear error. It must leave the folder's manifest marked `"complete": false` and must not touch the stores. Owner: Task 10.
3. **Calling `eval.run` while a capture starts, and two replays at once.** The speech replay must pause and the capture must not be degraded. A second `eval.run` must return `busy`. Owners: Tasks 11 and 12.
4. **A user who forgets a meeting or mail during an export or replay.** Forget must still remove the decision log entries. The export may contain the item only if it was written before the forget. Owner: Task 5, with a test that runs forget during an open log query.
5. **A day boundary and a restart for the quality counters.** Counts from before a restart are still sent. A day is never sent twice. "Untouched after 7 days" is counted once even if the client is off on day 7 and on again on day 9. Owner: Task 7.

---

### Task 1: Pin the current instruction text (golden tests)

Covers tasks.md 3.1. This is a pure refactor plus tests, with no behaviour change.

**Files:**
- Modify:
  - `client/src/triage/classifier.cpp:90-95`: move the system literal into `std::string triage::classification_instructions();`, declared in `classifier.h`.
  - `client/src/knowledge/knowledge_service.cpp:1378-1389` and `:1512-1524`: move the literals into the free functions `std::string knowledge::score_instructions();` and `std::string knowledge::resolve_instructions();`, declared in `knowledge_service.h`.
  - `client/src/agenda/service.cpp:370`: move the `model_merge` literal into `std::string agenda::merge_instructions();`, declared in `core.h`.
- Create:
  - `client/tests/fixtures/instructions/*.txt`: one file per instruction, captured from the current code.
  - `client/tests/eval/instructions_golden_test.cpp`, added to `win_tests`.

**Interfaces:**
- Produces:
  - `triage::classification_instructions()`;
  - `agenda::detection_instructions()`, which exists already;
  - `agenda::merge_instructions()`;
  - `knowledge::extraction_system_text()`, which is the system message of `extraction_request` with `kCommonSenseRubric` and is factored out of `:1028-1055`;
  - `knowledge::score_instructions()`;
  - `knowledge::resolve_instructions()`.

- [ ] **Step 1:** Write `TEST(InstructionsGolden, TextIsUnchanged)`, which compares each function with its fixture file byte for byte. Capture each fixture by copying the current literal exactly, including its escapes.
- [ ] **Step 2:** Build `win_tests`. It should fail to compile, because the functions are not defined yet.
- [ ] **Step 3:** Move each literal into its function. The call sites now call the function.
- [ ] **Step 4:** Run `win_tests.exe --gtest_filter=InstructionsGolden.*:Triage*:Agenda*:Knowledge*`. Expected: PASS.
- [ ] **Step 5:** Commit with the message `refactor: name model instructions and pin them with golden tests`.

### Task 2: Instruction registry and variants

Covers tasks.md 3.2 and design D3.

**Files:**
- Create:
  - `client/src/eval/registry.h` and `client/src/eval/registry.cpp`;
  - `client/tests/eval/registry_test.cpp`, added to the portable test target `eval_tests`, which is new and registered in `client/tests/CMakeLists.txt` like `triage_tests`.
- Modify the live readers:
  - `client/src/triage/triage.cpp`, which reads `kBatch` and `kMaxEventText` through the registry;
  - `client/src/triage/classifier.cpp`;
  - `client/src/agenda/service.cpp`;
  - `client/src/knowledge/knowledge_service.cpp`;
  - `client/src/meeting/sherpa_engine.cpp:289,335-336,429,431`, through parameters passed from the request, defaulting to the registry.

**Interfaces:**
- Produces:

```cpp
namespace eval {
enum class Step { triage, agenda, knowledge, diarize, asr };
std::string_view to_string(Step); std::optional<Step> parse_step(std::string_view);
class Variant {
 public:
  static Result<Variant> parse(Step step, const json& overrides); // rejects unknown names, out-of-range values, wrong step
  static const Variant& defaults(Step step);
  std::string text(std::string_view name) const;
  double number(std::string_view name) const;
  std::string choice(std::string_view name) const;
  std::string instruction_id() const;  // 12 hex chars, see Global Constraints
  Step step() const;
};
}
```

The registry entries, with their defaults and ranges:

| Step | Entry | Default | Allowed range or choices |
|---|---|---|---|
| triage | `triage.instructions` | text from Task 1 | — |
| triage | `triage.batch_size` | 20 | 1–20 |
| triage | `triage.body_limit` | 8192 | 1024–8192 |
| agenda | `agenda.instructions` | text from Task 1 | — |
| agenda | `agenda.merge_instructions` | text from Task 1 | — |
| knowledge | `knowledge.extraction` | text from Task 1 | — |
| knowledge | `knowledge.score` | text from Task 1 | — |
| knowledge | `knowledge.resolve` | text from Task 1 | — |
| diarize | `diarize.threshold` | 0.5 | 0.3–0.9 |
| diarize | `diarize.clusters` | `auto` | `auto` \| `attendees` |
| asr | `asr.hotword_score` | 2.0 | 0.0–5.0 |
| asr | `asr.beam_paths` | 4 | 1–8 |
| asr | `asr.vad_threshold` | 0.5 | 0.2–0.8 |

The design's `knowledge.rubric` is part of `knowledge.extraction`, because the two are always sent together.

Use `platform::sha256` and `hex_encode` (`platform/crypto.h:16`, `base/encoding.h:25`).

- [ ] **Step 1:** Write the tests:
  - `Registry.UnknownNameRejected`: `parse(triage, {"triage.instructons": "x"})` fails and the message contains `triage.instructons`.
  - `Registry.OutOfRangeRejected`: `{"triage.batch_size": 0}` fails, and so does `{"asr.beam_paths": 9}`.
  - `Registry.WrongStepRejected`: `parse(triage, {"diarize.threshold": 0.6})` fails.
  - `Registry.DefaultsMatchGolden`: `defaults(triage).text("triage.instructions") == triage::classification_instructions()`.
  - `Registry.InstructionIdStable`: the id is 12 hex characters, equal across two calls, and different when one override changes.
- [ ] **Step 2:** Run `eval_tests`. Expected: FAIL, because the symbols are not defined.
- [ ] **Step 3:** Implement `registry.cpp`. Then switch the live readers to `Variant::defaults(...)`. The speech engine takes the values as request parameters (see Task 12), with defaults from the registry, so the worker does not link policy.
- [ ] **Step 4:** Run `eval_tests` and `win_tests --gtest_filter=InstructionsGolden.*:Triage*:Meeting*`. Expected: PASS.
- [ ] **Step 5:** Commit with the message `feat(eval): instruction registry with variants and instruction ids`.

### Task 3: Pipe-only exposure, policy key and RPC gate

Covers tasks.md 2.1 and 2.2.

**Files:**
- Modify:
  - `client/src/host/rpc.h:15`: change the enum to `enum class RpcExposure { ui, ui_and_pipe, pipe };`.
  - `client/src/host/rpc.cpp:14`: `find` returns nullptr when `from_pipe` is set and the exposure is `ui`, and also when `from_pipe` is not set and the exposure is `pipe`.
  - `client/src/policy/core.cpp:134-182`: add `"evaluation": {"export": "off"}` to the defaults.
  - `control-plane/src/policy/keys.ts`: add the key `evaluation.export`, an enum of `off` and `on` with default `off`. Then regenerate `client/tests/fixtures/policy/schema.json` with `npm run policy:schema` in `control-plane/`.
- Create:
  - `client/src/app/eval_rpc.h` and `client/src/app/eval_rpc.cpp`;
  - `client/tests/eval/eval_rpc_test.cpp`, added to `win_tests`.
- Modify `client/src/app/app.cpp`: call `register_eval_rpc` inside `App::register_rpc()` (:601), next to `register_triage_rpc` (:2309).

**Interfaces:**
- Produces:
  - `void register_eval_rpc(RpcRegistry&, std::function<eval::EvalService*()> svc, std::function<bool()> enabled);`
  - The methods `eval.export`, `eval.run`, `eval.status` and `eval.cancel`, all registered with `RpcExposure::pipe`.
  - `bool eval::export_enabled(const policy::EffectivePolicy&)`, which is true only when the value is `"on"`.
- `eval::EvalService` is defined in Task 10. Until then the lazy getter may return nullptr, and calls answer `unavailable`.

- [ ] **Step 1:** Write the tests:
  - `Rpc.PipeOnlyHiddenFromWindow`: `find("x", false) == nullptr` for a method registered as `pipe`, and `find("x", true) != nullptr`.
  - `EvalRpc.OffMeansNotEnabled`: with `enabled = false`, each of the four methods returns the error code `not_enabled`, and the temp `out_dir` given to `eval.export` still does not exist afterwards.
  - `PolicyCore.EvaluationExportDefaultsOff`: the value is `"off"` when the key is missing, and an unknown value such as `"yes"` makes `export_enabled` return false.
  - The existing `PolicySchema.ClientDefaultsMatchTheGeneratedSchema` must pass.
- [ ] **Step 2:** Run `host` and `policy_tests`. Expected: FAIL.
- [ ] **Step 3:** Implement the code. Note that `dev.call` (app.cpp:826) uses `find(..., false)`, so it cannot reach pipe-only methods. That is intended; add a comment there saying so.
- [ ] **Step 4:** Run `policy_tests` and `win_tests --gtest_filter=Rpc*:EvalRpc*`. Expected: PASS.
- [ ] **Step 5:** Commit with the message `feat(eval): pipe-only RPC exposure and policy.evaluation.export`.

### Task 4: Settings shows that evaluation export is on

Covers tasks.md 2.3.

**Files:**
- Modify:
  - `client/src/inbox/inbox_service.cpp:688`: `settings.get` adds `"evaluation_export": true|false`.
  - `client/src/shell/views_pages.cpp:1178`: in `build_telemetry()`, add a warning line using the same pattern as `:1193`, with the automation id `evaluation-export-on`.
  - `client/src/ui/strings/{en,de,cs}.json`: add the key `settings.evaluation_export_on`:
    - en: "Evaluation export is on"
    - de: "Auswertungsexport ist eingeschaltet"
    - cs: "Export pro vyhodnocení je zapnutý"
- Test: `client/tests/shell/shell_test.cpp`.

- [ ] **Step 1:** Write the test `SettingsPage.ShowsEvaluationExportOn`. When the settings JSON has `evaluation_export: true`, the element `evaluation-export-on` exists with the en text. When it is false, the element is absent.
- [ ] **Step 2:** Run it. Expected: FAIL.
- [ ] **Step 3:** Implement the change.
- [ ] **Step 4:** Run `win_tests --gtest_filter=SettingsPage.*`. Expected: PASS.
- [ ] **Step 5:** Commit with the message `feat(eval): show evaluation export state in Settings`.

### Task 5: User decision log

Covers tasks.md 3b.1, 3b.2 and design D8.

**Files:**
- Create:
  - `client/src/eval/decision_log.h` and `client/src/eval/decision_log.cpp`;
  - `client/tests/eval/decision_log_test.cpp`, added to `win_tests`.
- Modify:
  - `client/src/storage/schema.cpp`: migration 2800 creates the table `decision_log(id INTEGER PRIMARY KEY, at INTEGER NOT NULL, step TEXT NOT NULL, kind TEXT NOT NULL, item_ref TEXT NOT NULL, result_ref TEXT NOT NULL, before_json TEXT NOT NULL, after_json TEXT NOT NULL, instruction_id TEXT NOT NULL)`, with the indexes `(item_ref)` and `(at)`.
  - Add `set_decision_log(eval::DecisionLog*)` to each of these services, then log from their handlers:
    - triage: `triage.h`/`triage.cpp` `dismiss` :806, `no_action` :817 and `start_work` :842;
    - Agenda: `agenda/service.cpp` `mark_done` :412, `dismiss` :422 and `edit` :442;
    - knowledge: `knowledge_service.cpp` `edit_fact` :4599, `delete_fact` :4624 and `set_fact_importance` :4648;
    - meetings: `meeting_service.cpp` `rename_speaker` :1060, `add_term` :1098 and `transcribe_again` :876.
  - `client/src/app/app.cpp`:
    - own the `DecisionLog` on `app.db` and inject it into the services;
    - register `forget.on_forget("item", "decision_log", ...)`;
    - call `log.forget_meeting` from meeting forget (`meeting_service.cpp:1126`);
    - prune once a day in `App::tick` (:481), using a stored last-prune day (`ticks % 3600` plus a day check).

**Interfaces:**
- Produces:

```cpp
namespace eval {
struct Decision {
  long long at_ms = 0; std::string step, kind, item_ref, result_ref, instruction_id;
  json before, after;  // each truncated to 2 KB serialized
};
class DecisionLog {
 public:
  explicit DecisionLog(storage::Database& app_db);
  Status record(const Decision& d);                   // call inside the caller's transaction for app.db decisions
  std::vector<Decision> for_item(const std::string& item_ref) const;
  std::vector<Decision> between(long long from_ms, long long to_ms) const;
  Status forget_item(const std::string& item_ref);    // also matches result_ref prefix "meeting:<id>/"
  int prune(long long now_ms);                         // deletes at < now - 365 days, returns count
};
std::string item_ref_mail(const std::string& id);      // "mail:<id>"
std::string item_ref_teams(const std::string& conv, const std::string& msg); // "teams:<conv>/<msg>"
std::string item_ref_calendar(const std::string& id);  // "calendar:<id>"
std::string item_ref_meeting(const std::string& id);   // "meeting:<id>"
}
```

- The `kind` values are:
  - `dismiss`, `no_action`, `start`;
  - `agenda_done`, `agenda_dismiss`, `agenda_edit`;
  - `fact_edit`, `fact_delete`, `importance`;
  - `speaker_rename`, `term_add`, `transcribe_again`.
- Triage entries put `{"action", "category", "suggestion"}` in `before`, and the instruction id stored in Task 8.
- Fact entries use `result_ref` `"fact:<kind>:<id>"`, and `item_ref` is the fact's first evidence item, or `""` if it has none. They are logged after the `kg.db` commit.

- [ ] **Step 1:** Write the tests:
  - `DecisionLog.RecordsEachKind`: one test per handler with the existing fakes (`Harness` in `tests/mail/triage_test.cpp:83`, the Agenda service fixture, `MailDb`). Each checks `kind`, `item_ref`, `before` and `after`.
  - `DecisionLog.LaterDecisionKeepsEarlier`: dismiss, then re-evaluate the revision, then `start_work`. `for_item` returns 2 entries in time order.
  - `DecisionLog.ForgetRemovesEntries`: forget an item, then a meeting; `for_item` is empty afterwards.
  - `DecisionLog.PruneAfter365Days`: entries at now−366 days and now−10 days; `prune` returns 1.
  - `DecisionLog.TruncatesLargeValues`: a `before` of 10 KB is stored as at most 2 KB.
  - `DecisionLog.ForgetDuringQuery` (Review Focus 4): forget runs between two `between()` pages, and the second page has no forgotten entries.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement migration 2800, `DecisionLog` and the handler calls. In `app.db` handlers, call `record` before the handler's commit.
- [ ] **Step 4:** Run `win_tests --gtest_filter=DecisionLog.*:Triage*:Agenda*:Knowledge*:Meeting*`. Expected: PASS.
- [ ] **Step 5:** Commit with the message `feat(eval): user decision log`.

### Task 6: Record who named a speaker

Covers tasks.md 4.2a.

**Files:**
- Modify:
  - `client/src/storage/schema.cpp`: migration 2801 adds `ALTER TABLE meetings ADD COLUMN speaker_meta TEXT NOT NULL DEFAULT '{}'`.
  - `client/src/meeting/meeting_store.h:28` and `meeting_store.cpp:41,55-56`: `MeetingRecord` gains `std::map<std::string, SpeakerMeta> speaker_meta;`. Leave the `speakers` map unchanged so existing readers keep working.
  - `client/src/meeting/meeting_service.cpp`: the callers of `name_speakers` (:597, :706) set `{by: "auto"}` for each name they create. `rename_speaker` (:1060) sets `{by: "user", at_ms: now}`.
- Test: `client/tests/meeting/meeting_test.cpp`.

**Interfaces:**
- Produces: `struct SpeakerMeta { std::string by = "unknown"; long long at_ms = 0; };`, where `by` is one of `auto`, `user` or `unknown`.

- [ ] **Step 1:** Write the tests:
  - `MeetingSpeakers.AutoNamesMarkedAuto`;
  - `MeetingSpeakers.RenameMarkedUserWithTime`;
  - `MeetingSpeakers.OldRowsReadUnknown`, which inserts a row before migration 2801 and checks that every speaker reads `by == "unknown"`.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement the change.
- [ ] **Step 4:** Run `win_tests --gtest_filter=MeetingSpeakers.*:Meeting*`. Expected: PASS.
- [ ] **Step 5:** Commit with the message `feat(meeting): record whether a speaker name came from the user`.

### Task 7: Daily quality counters

Covers tasks.md 3b.3 and design D9, plus the telemetry delta.

**Files:**
- Modify:
  - `telemetry/catalogue.yaml`: add the event `quality.daily` (catalogue names are dotted) with these attributes:
    - `step`, an enum: triage, agenda, knowledge, meeting;
    - `counter`, an enum (the list below);
    - `value`, an int;
    - `instruction_id`, a string with the pattern `^[0-9a-f]{12}$|^unknown$`;
    - `category`, an enum: `kCategories` plus `none`;
    - `action`, an enum: required, info, none.
  
  Then regenerate `catalogue.gen.h` with `control-plane/scripts/telemetry-catalogue.ts`.
  - `client/src/storage/schema.cpp`: migration 2802 creates:
    - `quality_counts(day TEXT, step TEXT, counter TEXT, category TEXT, action TEXT, instruction_id TEXT, value INTEGER, PRIMARY KEY(day, step, counter, category, action, instruction_id))`;
    - `quality_marks(kind TEXT, ref TEXT, PRIMARY KEY(kind, ref))`.
  - The places that increment counters:
    - triage: `triage.cpp` when a suggestion is created, in the decision handlers (shared with Task 5), and on batch failure by kind in `step()` (:537);
    - Agenda: admit and the decisions;
    - knowledge: facts stored, and the decisions;
    - meetings: transcription finished (meetings and minutes, speakers, the unclear share in per-mille), renames, terms, transcribe again.
  - `client/src/app/app.cpp` `tick()`: once a day, call `QualityCounters::sweep_week_old(now)`, then `flush(telemetry, today)`.
- Create:
  - `client/src/eval/quality.h` and `client/src/eval/quality.cpp`;
  - `client/tests/eval/quality_test.cpp`, added to `win_tests`.

**Interfaces:**
- Produces:

```cpp
namespace eval {
class QualityCounters {
 public:
  explicit QualityCounters(storage::Database& app_db, std::function<long long()> now_ms);
  void add(std::string_view step, std::string_view counter, std::string_view instruction_id,
           long long value = 1, std::string_view category = "none", std::string_view action = "none");
  void sweep_week_old(long long now_ms);  // triage suggestions open >7 days, auto-named "Speaker N" >7 days after meeting end; once each via quality_marks
  int flush(telemetry::TelemetryService& t, const std::string& today);  // emits rows with day < today, deletes them, returns events emitted
};
}
```

- The counter names are:
  - triage: `suggestion_created`, `dismissed`, `no_action`, `started`, `untouched_7d`, `batch_failed_unavailable`, `batch_failed_cut_off`, `batch_failed_mismatch`, `batch_failed_other`;
  - Agenda: `agenda_admitted`, `agenda_done`, `agenda_dismissed`, `agenda_edited`;
  - knowledge: `facts_stored`, `fact_edited`, `fact_deleted`, `fact_marked`;
  - meetings: `meetings_transcribed`, `minutes_transcribed`, `speakers_detected`, `speakers_unnamed_7d`, `speakers_renamed`, `terms_added`, `transcribed_again`, `unclear_permille`.

- [ ] **Step 1:** Write the tests:
  - `Quality.FlushEmitsPreviousDaysOnly`: today's rows stay in the table.
  - `Quality.SurvivesRestart`: add, destroy the object, construct a new one, flush; the count is still emitted.
  - `Quality.NeverSentTwice`: calling flush twice emits once.
  - `Quality.UntouchedCountedOnce`: run the sweep on day 9 and day 10; the item is counted once.
  - `Quality.OffLevelSendsNothing`: with the telemetry level off, flush emits 0 and keeps the rows until the next day they can be dropped.
  - `Quality.AttributesAllowList`: serialize the emitted event; its attribute keys are exactly the catalogue set, and no value contains `@`, a space-separated name or an id longer than 12 characters.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement the change. At level off, `flush` deletes rows older than 7 days without sending them.
- [ ] **Step 4:** Run `telemetry_tests` and `win_tests --gtest_filter=Quality.*`. Expected: PASS.
- [ ] **Step 5:** Commit with the message `feat(telemetry): daily content-free quality counters`.

### Task 8: Store the instruction id on new results

Covers tasks.md 3.3.

**Files:**
- Modify `client/src/storage/schema.cpp`, in the same 2800 block as a separate migration 2803:
  - `ALTER TABLE triage_items ADD COLUMN instruction_id TEXT NOT NULL DEFAULT 'unknown'`;
  - the same column on `agenda_items`.
- Modify `kg.db`: no migration. Facts carry the id in their existing provenance JSON under the key `instruction_id`, and its absence means `unknown`.
- Modify the writers in `triage.cpp` (the classify result write), `agenda/service.cpp` (admit) and `knowledge_service.cpp` (extract write).

- [ ] **Step 1:** Write the test `InstructionId.StoredOnNewResults` for each of the three, with fakes. Also write `InstructionId.OldRowsUnknown`.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement the change.
- [ ] **Step 4:** Run them. Expected: PASS.
- [ ] **Step 5:** Commit with the message `feat(eval): record instruction id on triage, Agenda and facts`.

### Task 9: Exporter for mail, Teams and calendar

Covers tasks.md 4.1, 4.4 and design D2.

**Files:**
- Create:
  - `client/src/eval/jsonl.h` and `client/src/eval/jsonl.cpp`;
  - `client/src/eval/exporter.h` and `client/src/eval/exporter.cpp`;
  - the tests `client/tests/eval/exporter_test.cpp` and `client/fuzz/eval_jsonl_fuzz.cpp`.
- Modify `client/src/mail/mail_store.h` and `mail_store.cpp`: add `std::vector<StoredMessage> messages_between(long long from_ms, long long to_ms) const;`. It returns every message, withheld ones included, ordered by `received_at`, with no limit.

**Interfaces:**
- Consumes:
  - `MailStore::events_between` and `folders()`;
  - `own_mail_folder` (mail_sync.h:84);
  - `TeamsService::conversation_ids()`, `conversation()` and `messages()`;
  - `DecisionLog::for_item`;
  - the `triage_items`, `agenda_items` and kg facts readers.
- Produces:

```cpp
namespace eval {
class JsonlWriter {               // writes one json per line; replaces invalid UTF-8 / lone surrogates with U+FFFD; never throws
 public:
  static Result<JsonlWriter> open(const std::filesystem::path& file);
  Status write(const json& record);
  long long lines() const;
};
struct ExportRequest {
  std::filesystem::path out_dir; long long from_ms = 0, to_ms = 0;
  std::set<std::string> sources;  // subset of {"mail","teams","calendar","meetings"}
  bool audio = false;
};
struct ExportCounts { long long written = 0, withheld = 0; };
struct ExportEnv { /* references: MailStore, TeamsService, MeetingStore, triage db, agenda db, KgStore, DecisionLog, clock */ };
class Exporter {
 public:
  explicit Exporter(ExportEnv env);
  Result<std::filesystem::path> begin(const ExportRequest& r);   // creates <out_dir>/<YYYYMMDD-HHMMSS>/, fails if exists or not writable
  Result<ExportCounts> export_mail(const ExportRequest&, const CancelToken&);
  Result<ExportCounts> export_teams(const ExportRequest&, const CancelToken&);
  Result<ExportCounts> export_calendar(const ExportRequest&, const CancelToken&);
};
}
```

- Each record has the form `{ "id", "item": {...}, "skowork": {...}, "user": {...} }`.
- A mail record's `item` holds:
  - `from`, `to`, `cc`, `received_at`, `subject`;
  - `folder` (the folder name);
  - `conversation_id`;
  - `body` (the text);
  - `sent_by_user`, which is `own_mail_folder` for the item's folder;
  - `user_replied` and `user_replied_approximate: true`.
- `user_replied` is true when a message in an own folder has the same `conversation_id` and a later `received_at`.
- Withheld messages are only counted.
- `skowork` holds:
  - `triage` (outcome, action, category, reason, suggestion, instruction_id);
  - `agenda`, as a list;
  - `facts`, as a list of `{statement, status, relations: [{salience, durability, confidence}], instruction_id}`.
- `user` holds:
  - `decisions`, from the `DecisionLog`;
  - `legacy`, which holds the stored suggestion state with `at` taken from `triage_items.updated_at`, or `null`, as design D2 describes.

- [ ] **Step 1:** Write the tests:
  - `Exporter.WindowAndSources`: on `MailDb` and the Teams fake, export only mail and Teams; check the counts, and that there is no `calendar.jsonl`.
  - `Exporter.WithheldOnlyCounted`: a withheld message is absent and `withheld == 1`.
  - `Exporter.UserRepliedDerived`: a sent-items message in the same conversation 1 hour later gives `user_replied: true`.
  - `Exporter.SentMailMarked`.
  - `Exporter.IncludesDecisionsAndLegacy`.
  - `Exporter.StoresUnchanged`: compare a `SELECT` dump of `triage_items`, `agenda_items` and `decision_log` before and after the export.
  - `Exporter.FolderExistsFails` (Review Focus 2).
  - `Jsonl.HostileText` (Review Focus 1): a lone surrogate, NUL, a 5 MB body and a subject of newlines each give exactly one line that parses as JSON.
  - The fuzz target `eval_jsonl_fuzz`: random bytes as body and subject, and every line must parse.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement the change.
- [ ] **Step 4:** Run `win_tests --gtest_filter=Exporter.*:Jsonl.*`, then the fuzz target for 60 s. Expected: PASS, with no crash.
- [ ] **Step 5:** Commit with the message `feat(eval): export mail, Teams and calendar with results and decisions`.

### Task 10: Meetings export, manifest, and the `eval.export` job

Covers tasks.md 4.2 and 4.3.

**Files:**
- Create:
  - `client/src/eval/wav.h` and `client/src/eval/wav.cpp`;
  - `client/src/eval/eval_service.h` and `client/src/eval/eval_service.cpp`.
- Modify:
  - `client/src/eval/exporter.h` and `exporter.cpp`: add `export_meetings`, `audio_size` and `write_manifest`;
  - `client/src/app/eval_rpc.cpp`.
- Test: `client/tests/eval/exporter_test.cpp` and `client/tests/eval/eval_rpc_test.cpp`.

**Interfaces:**
- Consumes `meeting::read_track(path) -> Result<DecodedTrack>` (`audio_file.h:49`), plus `MeetingStore` and `SpeakerMeta`.
- Produces:

```cpp
namespace eval {
Status write_wav(const std::filesystem::path& file, const std::vector<int16_t>& pcm, int sample_rate);
// on Exporter:
Result<ExportCounts> export_meetings(const ExportRequest&, const CancelToken&);
Result<long long> audio_size(const ExportRequest&) const;    // bytes the WAVs would take
Status write_manifest(const std::filesystem::path& folder, const ExportRequest&, const std::map<std::string, ExportCounts>&, bool complete);
class EvalService {
 public:
  Result<json> start_export(const json& params);  // {out_dir, from, to (ISO), sources[], audio, dry_run} -> {job, folder?, audio_bytes}
  Result<json> start_run(const json& params);     // Task 11/12
  json status(const std::string& job) const;      // {state: running|done|failed|cancelled, step, done, total, error?}
  Status cancel(const std::string& job);
};
}
```

- A meeting record holds:
  - `segments`;
  - `speakers` and `speaker_meta`;
  - `attendees`;
  - `event`, the linked `cal_events` row with `subject`, `organizer` and `body`;
  - `vocabulary` and `language`;
  - `audio`, which is either `{"local": "audio/<id>/local.wav", "remote": ...}` or `"unavailable"`.
- The manifest holds `window`, `sources`, `audio`, `client_version`, `counts` per Source, `instruction_ids` (one per step, from `Variant::defaults`) and `complete`.
- The job runs on a background thread. The manifest is written first with `complete: false` and rewritten with `true` at the end.

- [ ] **Step 1:** Write the tests:
  - `Wav.RoundTrip`: synthetic PCM written as WAV and read back gives the same samples.
  - `Exporter.MeetingWithAudio`: a meeting with both tracks gives two WAV files.
  - `Exporter.MeetingAudioUnavailable`.
  - `Exporter.NoAudioUnlessAsked`.
  - `Exporter.ManifestCountsAndIds`.
  - `Exporter.IncompleteOnFailure` (Review Focus 2): make the folder read-only part-way; the manifest stays `complete: false` and the stores are unchanged.
  - `EvalRpc.ExportJobLifecycle`: start, then poll `eval.status` until `done`; also a cancel half-way leaves the records already written.
  - `EvalRpc.DryRunReportsAudioBytes`.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement the change.
- [ ] **Step 4:** Run `win_tests --gtest_filter=Wav.*:Exporter.*:EvalRpc.*`. Expected: PASS.
- [ ] **Step 5:** Commit with the message `feat(eval): meetings export, WAV audio, manifest and export job`.

### Task 11: Replay of triage, Agenda and knowledge

Covers tasks.md 5.1, 5.3, 5.4 and design D4.

**Files:**
- Create:
  - `client/src/eval/replay.h` and `client/src/eval/replay.cpp`;
  - `client/tests/eval/replay_test.cpp`, added to `win_tests`.
- Modify:
  - `client/src/triage/triage.cpp:192-211`: extract `std::string triage::format_event_text(const std::string& from, const std::string& subject, long long received_ms, const std::string& body, size_t limit);` and use it from `source_text`.
  - `client/src/agenda/service.cpp:245-259`: extract `model::ChatRequest agenda::detection_request(const std::vector<json>& items, const std::string& instructions);`.
  - `client/src/triage/classifier.h` and `classifier.cpp`: `classify` gains an overload that takes `const std::string& instructions`. The existing signature forwards the default.
  - `knowledge_service.cpp`: `extraction_request` gains an `instructions` parameter, with the default from the registry.
  - `eval_service.cpp`: add `start_run`.

**Interfaces:**
- Consumes:
  - `eval::Variant::parse`;
  - `triage::classify(model, responsibilities, events, instructions, cancel)`;
  - `agenda::detection_request`;
  - `knowledge::extraction_request(batch, contexts, instructions)`;
  - the client's `model::IChatModel` for role `small`, the same instance the live features use.
- Produces:

```cpp
namespace eval {
struct ReplayRequest { std::filesystem::path folder; Step step; json variant = json::object(); int max_items = 200; };
class Replayer {  // owned by EvalService; one at a time
 public:
  Result<std::string> start(const ReplayRequest& r);  // validates variant + max_items (1..2000) before first item; returns run id; Error "busy" if one is running
  json status() const; void cancel();
};
}
```

- Results go to `<folder>/runs/<run id>/results.jsonl`, one line per item as `{id, result}`, or `{id, error: {kind, detail}}` when it fails.
- `run.json` holds `{step, variant, instruction_id, started, finished, items, failures_by_kind, tokens_in, tokens_out}`.
- No store, budget table or notifier is touched: the replayer holds only the model and the folder.

- [ ] **Step 1:** Write the tests:
  - `Replay.TriageWritesResultsOnly`: on a synthetic export of 30 mails with `FakeGateway` (`tests/model/fake_gateway.h:37`), there are 30 lines. Dumps of `triage_items`, `threads`, `agenda_items` and the triage `budget` are unchanged.
  - `Replay.VariantRejectedBeforeFirstItem`: the gateway receives 0 calls.
  - `Replay.MaxItemsBounds`: 0 and 2001 are rejected, and the default is 200.
  - `Replay.Busy` (Review Focus 3).
  - `Replay.CancelKeepsWritten`.
  - `Replay.LiveUnaffected`: while a variant replay runs, the live `TriageService` classifies a new item with the default instruction id.
  - `Replay.AgendaAndKnowledge`: the line counts and `run.json` fields are right.
  - Opt-in real-gateway test: `ReplayE2E.TriageAgainstGateway` in `tests/e2e/model_e2e_test.cpp`, labelled `e2e`.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement the change.
- [ ] **Step 4:** Run `win_tests --gtest_filter=Replay.*:Triage*:Agenda*:Knowledge*:InstructionsGolden.*`. Expected: PASS.
- [ ] **Step 5:** Commit with the message `feat(eval): replay triage, Agenda and knowledge with variants`.

### Task 12: Replay of diarization, naming and transcription

Covers tasks.md 5.2 and design D3/D4.

**Files:**
- Modify:
  - `client/src/meeting/sherpa_engine.cpp:289,335-336,429,431`: read `vad_threshold`, `num_clusters`, `threshold`, `max_active_paths` and `hotwords_score` from the request. Missing values use the registry defaults.
  - The `stt.*` request schema in `client/src/meeting/` (the `meeting::stt_request` handler): accept those optional fields, plus `wav_path` as an input next to the sealed track.
  - `client/src/eval/replay.cpp`: add the `diarize` and `asr` steps.
- Test: `client/tests/meeting/meeting_test.cpp` and `client/tests/eval/replay_test.cpp`.

**Interfaces:**
- Consumes `meeting::name_speakers(segments, user_name, attendees, {})` (`core.h:247`) and the meeting service's existing "capturing or transcribing" check, the same one batch transcription defers on at `meeting_service.cpp:530`.
- For `diarize.clusters = attendees`: `num_clusters` is the number of attendees other than the user when that number is at least 2, otherwise -1.
- The replay checks the capture or transcription state before each meeting and every 10 s. While busy, it sleeps and reports `state: "paused"`.

- [ ] **Step 1:** Write the tests:
  - `SttRequest.DefaultsUnchanged`: a request without the new fields produces the same engine config as before. Assert the config struct values: 0.5, -1, 4, 2.0, 0.5.
  - `SttRequest.OverridesApplied`.
  - `Replay.DiarizeTwoSpeakersSynthetic`: two synthetic voices from the test assets used by the existing meeting tests give 2 clusters and named speakers.
  - `Replay.AttendeesClusterCount`: 3 other attendees give `num_clusters == 3`, and 1 gives -1.
  - `Replay.PausesDuringCapture` (Review Focus 3): set the capture flag; the status goes to `paused` and no worker request is sent.
  - `Replay.SkipsMeetingWithoutAudio`.
- [ ] **Step 2:** Run them. Expected: FAIL.
- [ ] **Step 3:** Implement the change.
- [ ] **Step 4:** Run `win_tests --gtest_filter=SttRequest.*:Replay.*:Meeting*`. Expected: PASS.
- [ ] **Step 5:** Commit with the message `feat(eval): replay diarization, naming and transcription`.

### Task 13: Developer tooling (export script, labels, scoring)

Covers tasks.md 6.1, 6.2 and design D5/D6.

**Files:**
- Create:
  - `dev/scripts/eval.ps1`, with the parameter sets:
    - `-Export -From -To -Sources -Audio [-OutDir]`, where `OutDir` defaults to `<repo>/dev/eval-data`;
    - `-Run -Folder -Step [-Variant file] [-MaxItems]`;
    - `-Status -Job`;
    - `-Cancel -Job`;
    - `-Clean -Folder`, which asks for confirmation before deleting.

    It calls `skowork-pipe.ps1` and polls `eval.status` every 2 s.
  - `dev/eval/package.json` (Node 24 ESM, with scripts `check` (`tsc --noEmit`) and `test` (`node --test --test-concurrency=1 "test/**/*.test.ts"`)), copied from `dev/graph-mock`.
  - The `dev/eval/src/` files:
    - `labels.ts`: the label types from design D5;
    - `implicit.ts`: implicit labels from `user.decisions` and `user.legacy`;
    - `sample.ts`: the stratified sample;
    - `metrics.ts`;
    - `score.ts`;
    - `report.ts`;
    - `main.ts`.
  - `dev/eval/fixtures/synthetic-export/`: a small synthetic export, with no real data.
  - The tests `dev/eval/test/*.test.ts`.
- Modify `dev/.gitignore`: add `eval-data/`.

**Interfaces:**
- Consumes the export layout and `runs/<id>/` from Tasks 9 to 12.
- Produces these CLI commands:
  - `node dev/eval/src/main.ts implicit <folder>`, which writes `labels.jsonl` entries with `source: "user_decision"`;
  - `sample <folder> --mail 150 --teams 100`, which writes `sample.jsonl`;
  - `score <folder> <run>...`, which writes `report.md`.

`metrics.ts` exports:

```ts
export function precisionRecall(pred: boolean[], gold: boolean[]): { precision: number; recall: number };
export function wer(ref: string, hyp: string): number;            // word-level Levenshtein / ref words, lowercased, punctuation stripped
export function termErrorRate(ref: string, hyp: string, terms: string[]): number;
export function der(ref: Turn[], hyp: Turn[], collarMs = 250): number; // frame-based at 10 ms, optimal cluster mapping (Hungarian)
export function namingAccuracy(ref: Turn[], hyp: Turn[]): number; // share of speech time with the right person
```

- [ ] **Step 1:** Write the tests:
  - `wer("a b c", "a x c") === 1/3`;
  - `der` of identical turns is 0;
  - `der` with swapped labels is 0, through the optimal mapping;
  - `precisionRecall` on a fixed vector;
  - `implicit`: `no_action` gives `action: "info"` and `start` gives `"required"`;
  - `sample` is deterministic for a given seed and covers every category in the fixture;
  - `score` on the fixture gives a report with the D6 table rows;
  - the triage failure rate is read from `run.json`.
- [ ] **Step 2:** Run `cd dev/eval; node --test ...`. Expected: FAIL.
- [ ] **Step 3:** Implement the code. Then run `eval.ps1` against a debug build with the Graph mock (`dev/scripts/dev-stack.ps1`) for a smoke run: export, then run triage with `-MaxItems 5`.
- [ ] **Step 4:** Run `npm run check` and `npm test` in `dev/eval`. Expected: PASS. The smoke run must give `runs/<id>/results.jsonl` with 5 lines.
- [ ] **Step 5:** Commit with the message `feat(dev): evaluation export script and scoring tool`.

### Task 14: Spikes, baseline and closing

Covers tasks.md 1.1–1.3, 7.1–7.2 and 8.1. These are manual steps on the developer's instance.

- [ ] **Step 1 (spike 1.1):** Copy `app.db` and `kg.db` to `%TEMP%\skowork-backup-<date>\`. Run the debug build against `%LOCALAPPDATA%\Skowork`. It should open both stores, migrations 2800–2803 should apply, and the Inbox, Agenda and meetings should look the same as before. Record the result in tasks.md 1.1.
- [ ] **Step 2 (spike 1.2):** Turn on `evaluation.export` in the dev policy (`dev/policies`). Run `eval.ps1 -Export` for 90 days with all Sources and no audio. Record the time, the size and the counts in design D7. The target is under 5 minutes, and the Inbox must stay responsive.
- [ ] **Step 3 (spike 1.3):** Replay `diarize` and `asr` on the 54-minute meeting while you work normally. Record the wall time and CPU share. Start a capture during the replay and check that the replay shows `paused`.
- [ ] **Step 4 (7.1):**
  - Export with audio.
  - Run `implicit`, then `sample`.
  - The assistant drafts labels for the sample with `source: "drafted"` and a reason for each.
  - The user reviews the disagreements and the unsure items.
  - The user names the speakers and corrects 3 to 5 minutes per meeting.
- [ ] **Step 5 (7.2):** Run every step with variant `{}`, twice. Run `score`. Record the baseline in design D7. List the 10 worst triage items (by id only) and the naming errors as input to the follow-up changes. Check that the two runs are within ±3 points on each rate.
- [ ] **Step 6 (8.1):** Run `openspec validate --all --strict`, then `openspec archive add-evaluation-export --yes`, then reread `openspec/specs/client/evaluation/spec.md` and `openspec/specs/client/telemetry/spec.md`. Archive only after add-telemetry-and-admin has been archived.
