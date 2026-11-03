# Tasks

## 1. Validation spikes

- [ ] 1.1 Benchmark retrieval on the baseline laptop at 400k and 1M chunks with acme-llm-embedding vectors (binary coarse search plus int8 rescoring in kg.db, design D1; `client/tests/knowledge/kg_bench.cpp` measures the local part) and acme-llm-reranker through a prototype gateway. Test whether the `dimensions` parameter works, and compare retrieval quality at 1024 and 4096 dimensions on a cs/de/en query set. Verify that p95 hybrid query latency including both gateway calls stays under 1.5 s, and record storage per chunk and the chosen dimension in design.md
- [ ] 1.2 Evaluate entity and relation extraction by the on-prem `small` model on a labelled cs/de/en sample of 200 chunks. Verify precision and recall, and record the results in design.md
- [ ] 1.3 Verify a Graph user and manager lookup with the delegated User.Read.All scope (admin consent; design D5) through the organization Entra app registration

## 2. Store and ingestion core

- [x] 2.1 Create the `kg.db` schema (sources, items, chunks, FTS5, vec, entities, relations, mentions, user facts) with migrations. Verify with migration tests
- [x] 2.2 Implement Source enablement (policy defaults and user toggles) and exclusion handling (policy.knowledge.exclusions globs, mail-calendar exclusions, user exclusions), with removal on exclusion. Verify with unit tests for the policy-disabled, policy-glob and exclusion-added scenarios
- [ ] 2.3 Implement the indexer worker role with budgets (threads, priority, backfill throttling, daily extraction budget) and obedience to the client/desktop-host deferral signal. Verify that indexing stops on a simulated deferral, and that the core-process idle budget holds during backfill
- [x] 2.4 Register the forget-cascade handlers for Sources, items and entities. Verify that no derived rows remain after forgetting

## 3. Source connectors

- [x] 3.1 Implement the threads connector from client/agent-runtime Thread data. Verify that a finished Thread is indexed under the default policy
- [x] 3.2 Implement the files connector (folder watching, type and size filters), using client/core-tools document extraction for docx, xlsx, pptx and pdf, plus the md, txt and code readers in the worker. Verify with fixture files, and verify that fuzz targets exist for the new readers
- [x] 3.3 Implement the mail and calendar connectors consuming client/mail-calendar events. Verify against dev/graph-mock, including excluded-label items never being indexed
- [x] 3.4 Define the meetings connector interface for client/meeting-capture transcripts. Verify with a fixture transcript

## 4. Embeddings, extraction and resolution

- [x] 4.1 Implement chunking per content type, and embeddings through the `embed` role of client/model-access with the model id and dimension stored per vector. Verify with a test for the cross-lingual Czech-to-German retrieval scenario
- [x] 4.4 Implement the embedding queue: offline and quota queueing, the daily background embedding budget (policy.knowledge.embedding_daily_tokens) with new items first, and background re-embedding on a model or dimension change. Verify with a fake gateway for the indexing-while-offline, budget-reached and model-changed scenarios
- [x] 4.2 Implement batched extraction through client/model-access with quota backoff and resumability, guarded by policy.knowledge.extraction. Verify that no gateway calls happen when extraction is off
- [x] 4.3 Implement people resolution, Graph directory enrichment with caching (guarded by policy.knowledge.directory_enrichment), and alias clustering for other entities. Verify with tests for the same-person and enrichment-disabled scenarios

## 5. Retrieval and tools

- [ ] 5.1 Implement hybrid retrieval (BM25, vector, RRF, graph expansion, reranking through the `rerank` role) with provenance and fallback to fused order. Verify citation completeness, the reranker-unavailable scenario, and p95 latency on the benchmark set
- [x] 5.2 Implement knowledge.search, knowledge.entity, knowledge.related, knowledge.timeline and knowledge.remember with their risk classes and per-passage provenance. Verify with tool tests, including the injected-instruction scenario in the eval suite
- [x] 5.3 Expose the knowledge tools to skills through the standard tool registry. Verify that a skill run receives filtered results

## 6. Settings UI and integration

- [x] 6.1 Add the Knowledge sources settings section (sources, folders, policy and user exclusions, status, pause, re-index, forget, entity merge and split). Verify with UI tests
- [ ] 6.2 Run an end-to-end test on the baseline laptop with the mock mailbox and a document set: ask about a project and verify a cited answer drawn from mail, documents and earlier Threads
- [x] 6.3 Run `openspec validate --all --strict` and fix findings

## Workflow follow-up

- Archive with `openspec archive add-knowledge-graph --yes` after review.
