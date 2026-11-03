# Design

## Context

The motivation is in proposal.md. Constraints:
- **CPU-only ML** on the baseline laptop (i5, 16 GB RAM, iGPU).
- **Single encrypted SQLite store** in the data root (client/local-storage).
- **Background work** runs in `workbench-worker.exe`, which exits when idle (client/desktop-host).
- **Mail and calendar events** come from client/mail-calendar.
- **Thread data** (Source threads) comes from client/agent-runtime.
- **Document text extraction** for Office and pdf comes from client/core-tools.
- **Meeting transcripts** come from client/meeting-capture.
- **Extraction, embeddings and reranking** use the `small`, `embed` and `rerank` model roles through client/model-access and the LLM gateway. By default these map to acme-llm-secondary, acme-llm-embedding (4096 dimensions) and acme-llm-reranker (add-agent-runtime design D0). No ML model for knowledge runs on the laptop.

## Goals / Non-Goals

**Goals:**
- Useful hybrid retrieval with citations within 1.5 seconds p95 of a query on the baseline laptop, including the query-embedding and rerank calls, at about 400k chunks (typical) and up to 1 million chunks.
- Continuous, low-impact incremental indexing.
- A full and verifiable forget cascade.

**Non-Goals:**
- A general graph query language for users.
- Cross-user knowledge sharing (added later for documents, folders and notes by add-knowledge-sharing).
- OCR.
- Images and audio. Audio arrives only as transcripts.

## Decisions

### D1. Storage: one `kg.db` with FTS5, vector tables and relational graph tables
- **Tables:** `sources`, `items` (with Source, external id, title, timestamps, ACL tag, content hash), `chunks`, `chunks_fts` (FTS5 with the unicode61 tokenizer and diacritic folding, which matters for Czech), `vec_models`, `vec_bin` and `vec_i8` (a 1-bit binary-quantised vector for coarse search and an int8 vector for rescoring, per chunk and embedding model, see D2 and the vector layout below), `embed_queue` (chunks waiting for embedding), `entities`, `entity_aliases`, `relations` (subject, predicate, object, confidence, valid_from, valid_to, chunk_id), `mentions` (chunk to entity) and `facts_user`.
- **Why:** one encrypted file and one transaction model, with no separate server process. Vector search cost is in D2.
- **Implementation (2026-10-26):** `kg.db` is its own encrypted file in the data root with its own key, beside `app.db`, so forgetting everything can drop it whole. sqlite-vec is not vendored in client/third_party, so the vectors are plain BLOB tables and the coarse Hamming scan and the int8 rescoring run in C++ over them. Vendoring sqlite-vec instead is a new third-party dependency and needs approval; the measurements below do not call for it.
- **Vector layout (kg.db migration 2, 2026-10-26):** `vec_models` gives each (model, dimension) a small key. `vec_bin` (WITHOUT ROWID, primary key model key then chunk) holds only the binary vectors, so the coarse scan is one ordered range read of small rows. `vec_i8` is a rowid table with a unique (model key, chunk) index, so each int8 vector stays in its leaf page instead of spilling into an overflow page of its own, and rescoring reads 200 of them by key. Deleting a chunk cascades to both; deleting a `vec_models` row drops a whole model's vectors after a re-embed. The first layout, one `chunks_vec` table with both vectors per row, scanned at about 13 µs a chunk (each binary vector sat between 1 KB int8 overflow pages) and took 6.7 KB per chunk at 1024 dimensions; migration 2 moves existing rows into the new tables.
- **Measured (spike 1.1 tooling, 2026-10-26):** `client/tests/knowledge/kg_bench.cpp` builds an encrypted kg.db of synthetic cs/de/en chunks (110 words, grouped in 2,000 topics, vectors near their topic's centroid), then runs the service's hybrid search for 200 queries with the query embedding and the reranker answered in process. Development workstation (i7-12700) limited to 4 cores, release build, warm file cache; gateway round trips excluded:

  | Chunks | Dimensions | kg.db per chunk | Hybrid search p95 (local) | Vector step p95 | Full-text step p95 | Core private bytes |
  |---|---|---|---|---|---|---|
  | 20k | 1024 | 3.6 KB | 15 ms | 13 ms | 1 ms | 16 MB |
  | 400k | 1024 | 3.6 KB | 216 ms | 212 ms | 2 ms | 17 MB |
  | 1M | 1024 | 3.6 KB | 555 ms | 545 ms | 3 ms | 18 MB |
  | 1M | 4096 | 7.3 KB | 1,737 ms | 1,744 ms | 3 ms | 42 MB |

  The vector step is the binary scan, linear in chunks and in dimensions (about 0.55 µs a chunk at 1024, 1.7 µs at 4096); the search does not hold the vectors in memory (private bytes stay flat, the file cache serves the scan). **At 4096 dimensions a 1M-chunk index misses the 1.5 s goal on its local steps alone**, before the two gateway calls, and takes about 7.3 GB. So 4096 dimensions only fit up to roughly 400k chunks; for 1M chunks the index needs 1024 dimensions (if the `dimensions` parameter works and quality holds, which spike 1.1 decides) or a scan split across threads. Retrieval quality on the synthetic set says nothing about real queries: spike 1.1 still needs the baseline laptop, real acme-llm-embedding vectors, both gateway calls and a labelled cs/de/en query set.
- **Alternatives considered:**
  - A separate vector database such as Qdrant or LanceDB: an extra process and footprint.
  - A dedicated graph database such as Kuzu: an extra engine for modest graph sizes.
  - Hosting the graph on the server: rejected by the local-only requirement.

### D2. Embeddings: on-prem model through the gateway (decided 2026-10-21)
- **Choice:** the `embed` role (default acme-llm-embedding, 4096 dimensions) through client/model-access, in batches of 16 chunks. The laptop runs no embedding model, which keeps the models package to speech-to-text only and frees the index worker from ML memory.
- **Storage per chunk:** each vector is stored twice: binary-quantised (4096 bits = 512 B) for a Hamming-distance coarse search, and int8 (4 KB) for rescoring the top 200. That is about 4.6 KB per chunk plus text: 200k chunks ≈ 0.9 GB, 400k ≈ 1.8 GB, 1M ≈ 4.6 GB, within the 10 GB default of policy.storage.max_size_gb.
- **Search cost:** the coarse scan reads 512 B per chunk at 4096 dimensions, then rescoring reads 200 int8 vectors. Measured on the workstation (D1): 1.7 s at 1M chunks with 4096 dimensions and 0.55 s with 1024, so the 50–100 ms per 400k chunks first estimated here was too low by about 5×.
- **Smaller vectors:** if the upstream model accepts the `dimensions` request parameter (Matryoshka truncation), 1024 dimensions would cut storage by 4×. Spike 1.1 tests retrieval quality at 1024 and 4096; the dimension is a client setting, and changing it triggers a re-index.
- **Cost:** a typical backfill of about 400k chunks of about 256 tokens is about 100M tokens. At the default policy.knowledge.embedding_daily_tokens of 10M, it completes in about 10 days, most recent items first. The gateway's embed budget (policy.llm.budget.embed_daily_tokens, default 12M) sits above it, so query embeddings keep working during backfill.
- **Offline:** chunks wait in `embed_queue` and are found by full-text and graph expansion until they are embedded.
- **Alternatives considered:**
  - Local CPU model (multilingual-e5-small int8, about 120 MB, 384 dimensions, about 150–300 chunks/s per thread): no network or quota dependency, but it costs about 120 MB of models package and up to 300 MB of worker RAM, and its retrieval quality is lower than the on-prem model's.
  - Hybrid (local model for backfill, gateway for queries): vectors from two models are not comparable, so this would need two indexes.
  - float32 or float16 storage: 16 KB or 8 KB per chunk, too large at 1M chunks.
- **Upgrade path:** the model id and dimension are stored with each vector. A response with a different model id or dimension starts a background re-embed (spec: Embedding model changed). Vectors of two models are not comparable, so during the re-embed semantic search uses the vectors of the model that embedded the query; chunks not yet re-embedded are still found by full-text search and graph expansion. When the queue is empty, the new model becomes the active one and the old vectors are deleted.

### D3. Chunking
- **Approach:** structure-aware. Documents split by headings and paragraphs into about 256 tokens with a 32-token overlap (measured as about 1,024 and 128 characters). Email splits by message, with quoted history stripped. Transcripts split by speaker turn, in windows of about 2 minutes. Code splits by top-level symbol, falling back to lines.
- **Text extraction:** reuses the document text extraction of client/core-tools (sandboxed worker, fuzzed). This change adds only the plain-text and source-code readers and the chunkers.

### D4. Extraction pipeline
- **Batching:** chunks are batched (about 8 per request) to the `small` model with a JSON-schema-constrained prompt that returns entities, relations and dates.
- **Quotas:** extraction respects gateway quotas, backs off on rate limits and is resumable.
- **Prioritisation:** new and recently touched items first; backfill last.
- **Why an LLM rather than local NER:** local multilingual NER models are weak on relations and on project and system names. The on-prem LLM is already available.
- **Cost estimate:** about 1 extraction call per 8 chunks. The daily cap policy.knowledge.extraction_daily_calls is specified in client/knowledge-graph (default 2,000).

### D5. Entity resolution
- **People:** keyed by lower-cased SMTP address or user principal. When policy.knowledge.directory_enrichment is on, they are enriched through Microsoft Graph `/users/{upn}` (displayName, department, jobTitle) and `/users/{upn}/manager`, with the delegated Graph token of client/mail-calendar. Results are cached for 7 days, and only people in the user's own mail domain are looked up.
- **Scope (2026-10-26):** User.ReadBasic.All returns only display name, names, mail and photo; department, jobTitle and the manager need **User.Read.All**, which needs admin consent. client/mail-calendar requests it only while enrichment is on (its Least-Privilege Scopes). Spike 1.3 verifies it against the organization app registration. A Kerberos-bound LDAP lookup was rejected on 2026-10-22 together with Kerberos identity (fragile, needs domain-controller connectivity).
- **Other entities:** clustered by normalised name (case, diacritics and punctuation folded) within an entity type. A name-only person such as "Jana N." joins a Person only when exactly one known person matches the first name and initial. Embedding similarity of names is not used: vectors exist for chunks, not names, and embedding every name would spend the daily budget. Anything not merged automatically is left for the user.
- **User correction:** users can merge or split entities manually from the entity view.

### D6. Retrieval
1. FTS5 BM25 returns the top 50.
2. The query is embedded with the `embed` role, and the vector tables return the top 50 (binary coarse search, int8 rescoring of the best 200). If the query cannot be embedded, this step is skipped.
3. The two lists are merged with reciprocal rank fusion.
4. Graph expansion of 1–2 hops adds items that mention entities linked to the query's recognised entities.
5. The top 30 are reranked with the `rerank` role (default acme-llm-reranker). If reranking fails, the fused order is kept.
6. Results are returned with provenance.

- **Latency budget:** local steps about 300 ms at 400k chunks, plus two gateway round trips (query embedding and rerank) of about 200–500 ms each; spike 1.1 measures the total against the 1.5 s goal.
- **Alternatives considered:** a local MiniLM-class cross-encoder (int8, about 25 MB) avoids one round trip, but adds models package size and worker memory, and is weaker on Czech and German than the on-prem reranker.

Retrieved passages carry their provenance. client/agent-runtime wraps them in its untrusted-content delimiters, and client/policy-enforcement defines what they cannot do.

### D7. Budgets on the baseline laptop
- **Threads:** at most 2 threads (policy.knowledge.max_threads) at BELOW_NORMAL priority. The index worker reads, decodes and chunks files under a Job Object CPU rate cap of 50%. Gateway calls (embedding, extraction) and directory lookups need the model access and the Graph token of the core process, so they run on one below-normal background thread there; it sends at most one batch at a time and pauses 250 ms between backfill steps.
- **Measured (2026-10-26, workstation):** a backfill of 300 files grew the core process's private bytes by about 2 MB. The idle budget on the baseline laptop is measured in task 6.2.
- **Pausing:** the worker obeys the deferral signal of client/desktop-host (battery saver, presentation or busy state), and exits when idle as client/desktop-host specifies.
- **Typical initial backfill:** 20k emails plus 5k documents (about 400k chunks) are chunked and full-text indexed in under an hour of background time. Embedding completes in about 10 days within the daily embedding budget (D2), and extraction over several days within the daily call budget.
- **Memory:** with no ML model in the index worker, its peak RAM is expected to be about 250 MB (chunking, FTS5 and vector writes), within the ≤ 512 MB index-worker budget of client/desktop-host.

## Risks / Trade-offs

- [Extraction quality of the on-prem small model is poor for Czech] → Evaluate on a cs/de/en sample in spike 1.2. The spec allows extraction off, with search still working.
- [Prompt injection through indexed content] → Untrusted envelope, risk-class approvals (client/policy-enforcement) and egress rules. Red-team cases are added to the eval suite.
- [Chunk text leaves the laptop for embedding and reranking] → It goes only to the organization gateway and LLM API, the same path as LLM context. The gateway never records embedding or rerank inputs (control-plane/llm-gateway, Usage Audit), and nothing is stored server-side.
- [Fleet-wide backfill overloads the embedding backend] → Rollout is staged by Group, the per-user daily embedding budget caps each laptop, the gateway's per-backend capacity limits apply, and spike 1.3 of add-agent-runtime measures embedding throughput to size them.
- [Semantic search is unavailable offline] → Full-text and graph expansion still work, and queued chunks are embedded on reconnect.
- [kg.db grows large and hits the storage quota] → Store binary plus int8 vectors (D2), test 1024-dimension vectors, keep no raw files (text only), honour the quota from client/local-storage, and show per-Source size in the status view.
- [Directory enrichment leaks the directory into the local store] → Only name, department, title and manager are stored for people the user has interacted with, and they are deleted by the forget cascade.
- [Background CPU use annoys users] → Conservative defaults, pause controls, and telemetry on worker CPU minutes (metadata only).

## Open Questions

- Resolved 2026-10-21: embeddings and reranking use the on-prem models through the gateway (D2, D6).
- Does acme-llm-embedding support the `dimensions` parameter, and is 1024-dimension retrieval quality good enough? Spike 1.1 decides; this changes only a client setting.
