# Proposal

## Why

The desktop still calls itself Workbench, its microphone cannot be made usable from the shipped application, and administrators cannot discover individual tool decisions such as `computer.click`. Long conversations need earlier, reliable compaction, while users need to understand, correct and share the knowledge derived from their Sources without losing control when those Sources change.

## What Changes

- Rename the desktop and control-plane product surfaces to **SkoWork**, including the executable and distributed desktop package, while preserving existing profiles, credentials, configuration and integration identifiers.
- Make every supported policy setting and Tool discoverable in the control plane. Searching `computer.click` opens its attended and unattended decisions, inherited risk defaults, feature prerequisites and effective-policy explanation. Verify completeness against the client registrations, policy schema and owning specs.
- Deliver a versioned speech package with each desktop release and a **Set up speech recognition** flow with download, offline package import, progress, verification, repair and an actual recognition check. Keep transcription local and the lightweight core package separate.
- Replace the existing 85% compaction trigger with model-aware working budgets: at most 500,000 input tokens for models with at least a 1,000,000-token window, and at most 200,000 for smaller large-context models, further reduced for smaller windows and output reserves. Begin compaction before the budget, preserve complete conversational coverage and tool-call boundaries, and retain the readable Thread history.
- Rebuild Knowledge around **Sources, Knowledge, Review, and Sharing**. Users can follow each fact to its evidence, inspect possible contradictions, review source-change effects, and organize knowledge into editable categories with automatic suggestions.
- Add versioned evidence and dependency tracking. Updating or removing a Source recomputes dependent facts, categories, conflicts and shared publications; a fact with other valid evidence survives, and a fact with no valid support stops being used as current knowledge.
- Let users publish a reviewed selection from a category to their permitted Groups or the configured Company Group. The sharing preview shows the exact text, evidence excerpts, recipients and exclusions. Automatic categorization cannot expand access. New or changed category content waits for review; existing explicit document/folder live shares remain visible as a distinct sharing mode.

## Capabilities

### New Capabilities

- `client/speech-models`: speech package availability, acquisition, verification, repair and lifecycle shared by Dictation and meeting capture.

### Modified Capabilities

- `client/desktop-host`: SkoWork desktop identity, compatibility and packaged speech availability. This capability currently lives in the in-flight foundation change.
- `client/local-storage`: complete forget cascades, recovery-copy invalidation and minimal durable remote-withdrawal state. This capability currently lives in the in-flight foundation change.
- `client/dictation`: actionable missing-model and microphone states, with a working route back to the composer.
- `client/agent-runtime`: proactive, bounded and recoverable compaction for all Runs, including child Runs.
- `client/knowledge-graph`: inspectable knowledge, evidence, categories, conflicts, impact previews, lifecycle and category publication.
- `control-plane/admin-console`: SkoWork console identity, complete policy discovery and inspectable shared publications.
- `control-plane/policy-service`: tool catalog and policy coverage contract. This capability currently lives in in-flight policy changes.
- `control-plane/team-knowledge`: versioned category publications, authorization and withdrawal. This capability currently lives in the in-flight sharing change.

## Impact

- Proposed roadmap position **36**, after `redesign-admin-console-records` and `fix-desktop-machine-config`; implementation is split into independently testable task groups, with Knowledge delivered after its data lifecycle is proven.
- Builds on `add-client-foundation`, `add-policy-and-sandbox`, `add-agent-runtime`, `add-work-inbox`, `add-computer-use`, `add-meeting-capture`, the archived `add-dictation`, `add-knowledge-graph`, `add-knowledge-sharing`, `improve-knowledge-view`, `add-save-as-note` and `use-idp-tokens-for-mcp`.
- Several implemented foundations are still in flight. The effective baseline is the accepted specs plus those named changes in roadmap order. Before archival, reconcile and sync the prerequisites' applicable requirements without marking unfinished tasks complete; then merge this change last. Preserve all unrelated working-tree edits.
- Client impact: app resources/build/package scripts; native shell views; speech engine and model paths; runtime context, compaction and storage; encrypted knowledge migrations, ingestion, retrieval, host RPCs and UI Automation tests.
- Control-plane impact: policy schema/catalog/simulation and admin forms; team knowledge storage, APIs, search and administration; release/dev-artifact workflows. All service identity remains central-IdP based.
- Deliberate revisions to existing constraints: permit acquisition of the optional speech data package from inside the application (not a general application updater); extend explicit sharing from document/note text to reviewed knowledge publications; update the product name and Policy bundle glossary. Record these in `openspec/config.yaml` during implementation while preserving its existing edits.
- User data remains private by default in the encrypted local store. Only explicitly selected publication content goes to team knowledge; extraction, embedding, reranking and compaction use the existing LLM gateway.
- **Compatibility:** `SkoWork.exe` and SkoWork zip names replace the displayed desktop distribution names. Deployment scripts receive a compatibility entry point for the old executable; profile paths, `WORKBENCH_*` configuration, service URLs, citation identifiers and database formats are not renamed as a cosmetic operation.

## Out Of Scope

- General software distribution, a new installer/updater, new identity providers, personal direct sharing, public publishing, or moving private knowledge to a server.
- A graph editor as the default UX, ontology authoring, new Source kinds, OCR, local knowledge models, or a promise that automated contradiction detection finds every inconsistency.
- Silently sharing newly categorized content, granting permissions through extracted text, replaying completed tool effects during compaction, or automatically rewriting original documents from extracted facts.
- Completing unrelated open changes, committing, pushing or publishing artifacts as part of proposal preparation.
