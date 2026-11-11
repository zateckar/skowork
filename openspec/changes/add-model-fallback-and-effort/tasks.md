## 1. Validation spike

- [x] 1.1 Check which reasoning_effort values the organization LLM API accepts per chat model, and their effect (dev/scripts/llm-effort-spike.mjs). Verify: results recorded in design.md

## 2. Gateway

- [x] 2.1 Model settings (D1, D2): migration for `reasoning_effort`, `fallback_model` and `fallback_after_s`; validation on save; the effort probe on save and on connection Test; settings in the admin API with audit records. Verify with tests against a local fake upstream: accepted and refused effort, unreachable upstream, clearing without a probe, fallback to itself, to an unknown model and to a model with its own fallback refused, Test reports a stale effort
- [x] 2.2 Effort on requests (D1): the stored effort replaces the client's value; a null effort leaves the body unchanged. Verify with tests that inspect the forwarded body
- [x] 2.3 Fallback and cooldown (D3, D4): the fallback wait as first-byte limit for M, the switch reasons, the fit check, F's effort, keep-alives continuing across the switch, and the in-memory cooldown. Verify with tests: slow first byte falls back; 503 and 429 fall back; CONTEXT_LENGTH, INVALID_REQUEST and a cut stream do not; a request too large for F keeps the error; cooldown sends the next request straight to F and expires; non-streamed fallback
- [x] 2.4 Fallback Audit (D5): `answered_by` and `fallback` columns. Verify with tests for each fallback reason

## 3. Admin console

- [ ] 3.1 Model editor in the Connections view: effort select, fallback picker (chat models, not the model itself, not models with their own fallback), wait field, the refused effort's reason next to the field, and effort and fallback in the model list. Verify in the browser against the local control plane with the real organization LLM API connection

## 4. Acceptance

- [x] 4.1 On the local stack with the real organization LLM API: set `low` on the primary model and check a chat request's reasoning tokens drop; set `high` on the secondary and see it refused; set the secondary as fallback of the primary with a 5-second wait, send a long request and check the audit shows `timeout` then `cooldown`. Record the results in design.md
- [x] 4.2 Run `openspec validate --all --strict`
