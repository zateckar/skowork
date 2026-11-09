# Tasks

All tasks are implementation work pending approval. Preserve the existing admin-console and machine-configuration edits. Store design decisions and verification evidence only under openspec/. Task numbers define delivery order, not permission to commit or publish.

## 1. Validation Spikes And Effective Baseline

- [x] 1.1 Compose the applicable accepted and in-flight requirements in a temporary OpenSpec workspace in roadmap order; verify every MODIFIED header and inherited scenario in this change merges correctly, and record the required prerequisite sync order in design.md without completing or archiving unfinished changes.
- [ ] 1.2 Prove the existing pinned speech assets can form one verifiable offline package within desktop-host budgets, and confirm download/proxy and worker initialization paths from an isolated packaged client; record real sizes, license inventory, compatible versions and English/Czech fixture output before implementing the setup UI.
- [ ] 1.3 Exercise the configured gateway's main/small metadata and bounded multi-pass summarization on a seeded conversation; verify oldest/middle/newest goals and untrusted provenance survive two passes, and record actual context limits, usage and the token-count strategy without exposing credentials.
- [ ] 1.4 Prototype revision/support-set migration and conflict/category analysis on a copied encrypted fixture database and a labeled cs/en/de corpus; verify independent versus required dependencies, date/scope distinctions, misleading source instructions and missing-model behavior, recording quality and baseline timing rather than assuming model correctness.

## 2. Specification And Compatibility Contracts

- [x] 2.1 Reconcile and sync only the approved prerequisite requirements identified in 1.1; update this change if a conflict exists, preserve accepted Save As Note behavior, and verify the composed runtime/knowledge/admin specs with `openspec validate --all --strict` before implementation.
- [x] 2.2 Update `openspec/config.yaml` for the product name, bounded speech-package acquisition exception, Category/Publication glossary, policy inventory and roadmap position 36 while preserving existing edits; verify every new policy has exactly one owner and every changed requirement has an acceptance scenario.
- [ ] 2.3 Define additive capability/schema negotiation for speech setup and publications; verify contract fixtures reject unsupported mutations and unknown mandatory keys while older clients retain ordinary item search/sharing.

## 3. SkoWork Product Identity

- [ ] 3.1 Rename visible desktop/admin identity, localizations, version resources and primary executable/package outputs; verify localized string checks, a release build and inspection of the packaged executable metadata show SkoWork without broken launch/sign/package paths.
- [ ] 3.2 Implement the legacy executable compatibility launcher and update own-process computer-use exclusions; verify supported arguments, existing-instance activation, both executable names being protected, and startup against a copied existing profile with Threads, knowledge and credentials intact.
- [ ] 3.3 Inspect the native window, tray, About, notifications, sign-in and admin title with the final build; verify UI Automation names and existing citations, and record screenshots/observations and core package footprint in design.md.

## 4. Complete Policy Discovery

- [ ] 4.1 Add owned policy descriptions, prerequisite metadata and the built-in Tool catalog to the existing registry; verify a bidirectional inventory against all client Tool registration paths, owning spec declarations and server/client key support, including all nine computer Tools and disabled capabilities.
- [ ] 4.2 Extend schema/catalog and simulation responses with attended/unattended decision traces and conditional dynamic-risk details; verify `policy.test.ts`, `policy-merge.test.ts` and matching C++ policy fixtures cover exact/custom/wildcard rules, guardrails, Group precedence, feature gates and destructive floors.
- [ ] 4.3 Replace free-text-only Tool-rule entry with searchable records/dialog/picker flows in the existing admin redesign; verify browser tests find unset computer.click, save approve, reset inheritance, preserve custom MCP rules, identify disabled computer mode and display effective versus draft results correctly.
- [ ] 4.4 Register all new policy keys and regenerate the client schema fixture; verify `npm run policy:schema -- --check`, the negative missing-catalog/key coverage tests, TypeScript check and admin build, then add the completeness gate to release validation.

## 5. Speech Package And Dictation Recovery

- [ ] 5.1 Consolidate pinned speech metadata and create the full runtime/models/license package with trusted manifest references in both release and dev workflows; verify a package layout test and clean offline import find every required asset without source-tree or environment fallbacks.
- [ ] 5.2 Implement acquisition/staging/verification/activation under the user-private data root with existing WinHTTP behavior; verify cancellation, restart, disk exhaustion, origin redirects, credential non-forwarding and corrupt/incompatible package handling, and fuzz archive/manifest parsing for traversal, links, duplicate entries and inflated sizes.
- [ ] 5.3 Implement versioned package selection, safe repair/removal and worker readiness probing; verify no active worker loses its files, failed activation preserves the prior package, cleanup stays in its resolved staging root and successful setup refreshes availability immediately.
- [ ] 5.4 Add Settings speech setup and composer recovery with clear model, permission, missing-device and capture-busy states; verify shell UI Automation tests preserve the draft/caret, never record from Install/Verify, handle unplug/reconnect and retain existing Dictation stop/cancel/language/privacy behavior.
- [ ] 5.5 Restrict transcription and language selection to Czech, English and German, treating previous unsupported dictation settings as Automatic; verify rejected languages and migration, then run real sherpa transcription from the packaged client for all three supported languages and an explicit microphone-check flow on a controlled test session. Verify editable text, no automatic send, deleted temporary audio, local-only audio and actual cold/warm latency and memory; record fixture-based versus hardware-microphone evidence separately.

## 6. Automatic Context Compaction

- [ ] 6.1 Implement one shared request-budget calculation and expose it in context usage; verify exact boundaries for 1,000,000, 1,048,576, 262,000, 262,144, smaller and unknown windows, large output reserves, UTF-8 text, tool-schema-heavy requests, images and model remapping.
- [ ] 6.2 Build versioned compaction snapshots including completed active-Run exchanges and isolated child-Run history; extend RuntimeCompaction tests to verify unresolved tool batches and the latest user instruction survive, completed side effects are never replayed and approvals remain authoritative.
- [ ] 6.3 Replace transcript truncation with bounded full-coverage summarization and provenance-preserving checkpoint data; verify every removed message and prior summary is covered, oversized individual messages are handled, injected source text remains untrusted and repeated summaries retain seeded early constraints.
- [ ] 6.4 Commit checkpoints atomically with revision fencing, idempotent hooks and restart recovery; verify cancellation, empty/failed summaries, new input during compaction, storage failure, repeated no-progress attempts and an input that cannot fit all leave readable history and no oversized request.
- [ ] 6.5 Wire proactive triggering and progress into native Thread views and Activity; verify the 450k/180k trigger behavior and 300k/120k targets with runtime tests, then run the real-gateway qualification from 1.3 on the final implementation, recording model usage, recall and actual latency.

## 7. Knowledge Evidence And Lifecycle

- [ ] 7.1 Add encrypted migrations for revisions, evidence spans, canonical facts, independent/required support sets, annotations and generation fences; extend KnowledgeStore tests to verify migration of existing relations, notes, corrections and tombstones, stable links, transaction rollback and rejection of incompatible old writers.
- [ ] 7.2 Implement reverse-dependency reconciliation for changed passages and independent support; verify two Sources supporting one fact, multi-input derivation, moved unchanged text, changed file content with unchanged size/mtime, transient access errors and analysis unavailable after a Source revision.
- [ ] 7.3 Extend forgetting/exclusion to derived evidence, conflict caches, previews, recovery copies and summary dependencies while retaining only encrypted withdrawal identity records; verify original files remain, no content is searchable after acknowledgment, re-indexing cannot revive forgotten items, a surviving independent support keeps the fact, and Delete all while offline can finish remote cleanup after the same user signs in again.
- [ ] 7.4 Use the same state/filter policy in knowledge tools and UI query APIs; verify rejected/superseded/unsupported facts are excluded from current answers, disputes return both sides, history is explicit, category filters work and all returned citations resolve to the correct permitted revision.

## 8. Categories, Consistency And Impact Review

- [ ] 8.1 Add automatic/manual category assignments, reasons, rejected suggestions and locked choices; verify multi-label assignment, manual precedence, merge/rename/delete without knowledge loss, Uncategorized fallback and categorization-off behavior.
- [ ] 8.2 Add bounded incremental inconsistency detection and evidence-bound resolutions using the small role; verify dated facts, ambiguous people, conflicting values/units, reopened resolutions and deferred/budget-limited analysis, and fuzz structured model output for invented ids/spans, cycles and oversized values.
- [ ] 8.3 Implement encrypted import staging and revision-bound impact previews for add/update/exclude/forget; verify preview counts match committed changes, a changed file/policy/graph forces refreshed confirmation, cancellation cleans staging, large imports paginate and incomplete analysis is explicitly labeled.
- [ ] 8.4 Add durable source-change reports and shared analysis-budget scheduling; verify offline full-text search, resume after budget reset/cancellation/restart, source-generation fencing and measured 10k-item/100k-fact list and dependency-diff targets on the baseline or explicitly identified substitute hardware.

## 9. Versioned Team Publications

- [ ] 9.1 Add bounded publication storage and endpoints with server-computed hashes, expected revisions and idempotent operations; extend `team-knowledge.test.ts` to verify atomic writes, two-device conflicts, quota changes during embedding, failure rollback, duplicate retries and unauthorized reads/writes; fuzz payload validation.
- [ ] 9.2 Re-evaluate owner policy and identity authority on publication queries and final write commit; verify Group removal, owner revocation, policy revocation, expired role authority, forbidden Source kinds and absence of unauthorized text/title/count leakage without an owner upload.
- [ ] 9.3 Add per-entry and whole-publication withdrawal barriers, admin removal and metadata-only audit; verify delayed uploads and old-client item APIs cannot resurrect content, partial removal cleans vectors/excerpts, unaffected entries survive and renewed publication needs the specified explicit action.
- [ ] 9.4 Add Company Group configuration and capability-compatible publication search/read responses; verify unset/forbidden Company cases, explicit everyone permission rather than wildcard expansion, older-client citations and existing document/note share behavior.

## 10. Knowledge Workspace And Publication Control

- [ ] 10.1 Build Sources/Knowledge/Review/Sharing on the native list/reader controls and new paginated RPCs; verify shell UI Automation tests traverse source-to-fact-to-excerpt in both directions, view source revisions, resolve a conflict, preserve narrow-window navigation and operate all dialogs by keyboard.
- [ ] 10.2 Implement category publication selection, policy/dependency eligibility and exact payload preview; verify mixed-source restrictions, copied mail/Chat origin notices, removal of private paths, multiple categories, Company audience explanation, cancellation and a byte-for-byte match between confirmed public content and uploaded entries.
- [ ] 10.3 Implement publication differences and the durable withdrawal-first outbox; verify new category members stay private, invalidated facts withdraw without model calls, offline removal is visibly pending, reconnect survives a crash, admin removal remains blocked and source/category edits cannot broaden an audience.
- [ ] 10.4 Extend the existing records/dialog admin Knowledge view to inspect exact publication versions and remove with a reason; verify browser E2E permissions, no private unpublished data, keyboard dialog behavior and the owner's synchronized removed state.

## 11. Final Integration Evidence

- [ ] 11.1 Run the relevant client debug/release CTest suites and the control-plane typecheck, build and full tests after the final edits; run strict OpenSpec validation and inspect the diff to verify unrelated existing work is preserved and all new code maps to this change's specs.
- [ ] 11.2 Exercise the complete lifecycle in isolated profiles: import two corroborating Sources and one conflicting revision, inspect/correct/categorize, publish to a Group, change/delete supporting Sources, disconnect/reconnect, revoke access and inspect from a second user; verify UI, API, database and agent retrieval agree at each step and record evidence in design.md.
- [ ] 11.3 Verify final packaged artifacts rather than only a development build: SkoWork startup with legacy compatibility, speech setup with no source-tree fallback, policy catalog completeness, compaction with the actual configured model and footprint checks. Record unverified hardware/provider/release checks explicitly; a local test does not establish a published release or a successful remote CI run.
- [ ] 11.4 Review every acceptance scenario against final evidence, reconcile any discovered spec gaps before fixes and rerun the affected checks; verify no task is checked merely because a compile or unrelated test passed.

## Workflow follow-up

- Once every tracked task is actually verified, run `openspec validate --all --strict` and `openspec archive improve-skowork-and-knowledge-control --yes`; reread the merged capabilities and rerun strict validation. Resolve predecessor sync warnings before archival rather than bypassing them.
- Commit, push and release publication remain separate user instructions. If publication is later requested, inspect the actual CI/release result for the final SHA and verify the downloadable desktop and speech artifacts before calling it complete.
