# Design

## Context

Today `control-plane/src/config.ts` builds the upstream list from WORKBENCH_LLM_UPSTREAMS or OPENAI_BASE_URL/OPENAI_KEY, and the model capabilities are the constant `defaultCapabilities`. The gateway resolves a role through policy.llm.models to a model id and sends it to the configured upstreams. The product owner decided on 2026-10-31 that connections are managed in the console.

## Decisions

### D1. Storage and encryption
- **Choice:** tables `llm_connections` (id, name, base_url, credential_enc, credential_hint, enabled, max_concurrent, position, status, status_detail, checked_at) and `llm_models` (connection_id, model_id, kind, context_window, max_output_tokens, tools, images, reasoning, dimension, last_seen, missing). The credential is encrypted with AES-256-GCM under a key from WORKBENCH_SECRET_KEY (base64, 32 bytes). Development without it generates a key file `secret.key` next to the database with owner-only permissions; production refuses to start without WORKBENCH_SECRET_KEY once a connection exists.
- **Alternatives considered:** storing the credential in the policy (signed bundles reach clients: never); an external secret store (none is available in the target environment yet; the column format allows moving later).

### D2. Bootstrap from environment
- **Choice:** at start, if `llm_connections` is empty and OPENAI_BASE_URL is set, insert `primary` with OPENAI_KEY, and WORKBENCH_LLM_UPSTREAMS entries the same way. Afterwards the environment is ignored for upstreams, so console edits are not overwritten by a restart. The code defaults for capabilities seed `llm_models` rows for known ids.
- **Alternatives considered:** environment always wins (console edits lost on restart); no bootstrap (an empty gateway after upgrade).

### D3. Routing
- **Choice:** the gateway keeps an in-memory routing table rebuilt on every change: model id → ordered list of enabled connections that offer it. `<connection>/<id>` pins one connection. Failover between connections for the same id follows the existing backend failover rules, with each connection treated as a backend.
- **Alternatives considered:** one connection per role (cannot express failover).

### D4. Discovery
- **Choice:** `GET <base>/models` with the credential, 10 s timeout, on save, on "Test", and every 24 hours. Errors set `status = failing` with the HTTP status or network error; the model list is kept.

## Risks / Trade-offs

- [A console bug or a compromised admin account exposes credentials] → Credentials are write-only in every API, encrypted at rest, and every change is audited; only platform-admins can manage connections.
- [Losing WORKBENCH_SECRET_KEY makes stored credentials unreadable] → The gateway reports those connections as failing with "credential cannot be decrypted"; the admin re-enters the credential.
- [Discovery hits a slow upstream on save] → 10 s timeout; saving never waits longer, and status shows "checking".

## Acceptance notes (2026-11-01)

- The primary connection to the organization LLM API tested ok with 5 models listed; deleting it was refused with 409 IN_USE naming the server default roles.
- Publishing a Group mapping of main to acme-llm-secondary made the next Run of a member use it.
- Found: policy.llm.models merges as a whole map, so a Group mapping only main made every other role unavailable (small and embed were refused with model_unavailable) while the Models view showed them as inherited. Fixed in the view: it shows the inherited model of each role, and when a Group maps any role, roles left as inherited are saved with that model. The admin console e2e test checks it.
- Connections view in the console against the real organization LLM API:
  - add and test;
  - edit and the model list with capabilities;
  - disable after confirming, then enable;
  - delete, whose confirmation names the role mappings that use the connection.
- Found: the add form kept its values after a connection was saved. It is now cleared once the connection is saved, also when its test fails, so the same name is not submitted twice. The e2e test checks it.
