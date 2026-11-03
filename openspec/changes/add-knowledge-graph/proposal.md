# Proposal

## Why

To be useful, the agent must remember and connect what the user works on: past sessions, local documents, email, calendar and meetings. Today each Run starts nearly blind. A local, private knowledge graph with semantic search gives the agent grounded, cited context, and it keeps the data on the laptop.

## What Changes

- Add a local knowledge store per user. It holds chunks, full-text and vector indexes, entities, relations and facts, all with provenance.
- Add ingestion from the Sources threads, files, mail, calendar and meetings. Each Source has policy defaults and user toggles, exclusions and size limits.
- Add embeddings from the on-prem embedding model through the gateway (`embed` role), with vectors stored locally, a daily background embedding budget, offline queueing and re-indexing on model change. No embedding model runs on the laptop.
- Add LLM-based extraction of entities and relations through the gateway. Extraction can be switched off by policy.
- Add people resolution by email and user principal, with enrichment from the organization directory.
- Add hybrid retrieval: full-text, vector, graph expansion and reranking with the on-prem reranker (`rerank` role). Each result carries provenance.
- Add the tools `knowledge.search`, `knowledge.entity`, `knowledge.related`, `knowledge.timeline` and `knowledge.remember`, also available to skills.
- Add background indexing budgets suited to the baseline laptop, plus status and forget controls.

Out of scope:
- Sharing knowledge between users or syncing it to the server. Knowledge never leaves the laptop, except as LLM context through the gateway. (add-knowledge-sharing later lets users share chosen documents, folders and notes with Groups.)
- Indexing network shares or SharePoint and OneDrive cloud-only files.
- OCR of scanned documents and images.

## Capabilities

### New Capabilities
- `client/knowledge-graph`: knowledge Sources, ingestion, embeddings, extraction, entity resolution, hybrid retrieval, knowledge tools, indexing budgets and data control

### Modified Capabilities
- None.

## Impact

- **Roadmap:** change 12 of 13 (Knowledge).
- **Depends on:**
  - add-client-foundation (storage, forget cascade, worker model)
  - add-agent-runtime (tools; model roles `small`, `embed` and `rerank`; the gateway's embedding budget)
  - add-policy-and-sandbox (policy.knowledge, file grants)
  - add-work-inbox (settings presentation)
  - add-core-tools-and-mcp (document text extraction for Office and pdf)
  - add-mail-calendar (mail and calendar events, mail exclusions)
  - add-skills (Skills use the knowledge tools under the same policy decisions)
- **Forward reference:** client/meeting-capture (change 13) provides the Source meetings. Until it lands, that Source is empty.
- **New code:**
  - the client knowledge module and `kg.db` schema;
  - the indexer role of `workbench-worker.exe`;
  - plain-text and code readers and chunkers. These parse untrusted input and are fuzzed.
- **Network:** with directory enrichment on, Microsoft Graph user lookups through the existing Graph platform connection of client/mail-calendar; no new hosts.
- **Artifacts:** none. Knowledge needs no local ML model.
- **Load:** extraction calls to the `small` model, and embedding and rerank calls, add load on the LLM gateway and the organization LLM API. A typical backfill is about 100M embedding tokens per user, spread over about 10 days by policy.knowledge.embedding_daily_tokens; quotas apply, and rollout is staged by Group.
- **Policy keys owned here:** policy.knowledge.* (sources, exclusions, max_file_mb, extraction, extraction_daily_calls, embedding_daily_tokens, directory_enrichment, max_threads).
