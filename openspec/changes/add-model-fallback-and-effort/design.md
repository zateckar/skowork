# Design

## Context

- **Model settings.** Since add-llm-connections, each discovered model of a connection has stored settings in `llm_models`: kind, context window, maximum output, tools, images, reasoning and dimension. Platform-admins edit them in the Connections view.
- **Routing.** The gateway resolves a role through policy.llm.models to a model reference (`id` or `connection/id`). It then fails over between the enabled connections that offer that id.
- **Stream limits.** fix-long-thread-model-calls adds keep-alives and a first-byte limit of 180 s to streamed requests.

### Spike (2026-11-10, `dev/scripts/llm-effort-spike.mjs`)
Each model was sent one arithmetic question, max_tokens 4000, temperature 0. The correct answer is 13:54.

**`skoda-llm-primary`**

| reasoning_effort | Result | Time | Reasoning tokens | Answer |
|---|---|---|---|---|
| unset | 200 | 9.7 s | 1,348 | 13:54 |
| none | 200 | 0.45 s | 0 | 12:53 (wrong) |
| low | 200 | 2.0 s | 223 | 13:54 |
| medium | 200 | 13.2 s | 1,601 | 13:53 |
| high | 200 | 4.2 s | 542 | 13:54 |
| minimal | 400 | | | |

The 400 says: "This model supports thinking_effort values: low, medium, high, max".

**`skoda-llm-secondary`**

| reasoning_effort | Result | Time | Reasoning tokens | Answer |
|---|---|---|---|---|
| unset | 200 | 20.8 s | 3,153 | 13:54 |
| none | 200 | 0.57 s | 0 | 12:00 (wrong) |
| low | 200 | 7.3 s | 1,317 | 13:54 |
| medium | 200 | 7.1 s | 1,262 | 13:54 |
| high | 400 | | | |
| minimal | 400 | | | |

The 400 says: "Supported types are xhigh (default), medium, and low".

So the secondary model also reasons. add-agent-runtime D0 recorded it as non-reasoning, which is no longer true. The accepted values differ per model, and an unsupported value makes every request fail with 400.

## Decisions

### D1. Reasoning effort is a model setting, checked on save
`llm_models.reasoning_effort` is null (the model's own default) or one of `none`, `minimal`, `low`, `medium`, `high`, `xhigh` or `max`.

**Check on save.** Before saving a value, the gateway sends a one-token chat request with that value to an enabled connection that offers the model. An upstream 400, 404 or 422 refuses the save with 400 INVALID_REQUEST: the upstream judges the value. The message names the value and includes the upstream's explanation, with credentials and addresses removed. Any other answer (401, 403, 429, 5xx) and a network failure say nothing about the value, so they refuse the save with "could not be checked". Setting the value back to null needs no check.

**On each request.** The gateway sets `reasoning_effort` in the forwarded body of every chat request routed to that model. It replaces any value the client sent, since the client sends none today, so the administrator's choice is authoritative. A model with a null setting gets the client's body unchanged.

**Alternatives considered**
- *A fixed list per model family:* the spike shows that models of the same organization differ, and a list would go stale when the upstream changes.
- *A policy key per role:* the effort belongs to the model, not to a Group's role mapping. A per-role key would also have to be repeated for the fallback model.
- *Let the client send it:* that needs a client change and a policy for who may choose. It is not needed now.

### D2. Fallback is a model setting
`llm_models.fallback_model` holds a model reference (`id` or `connection/id`), validated on save:
- it must resolve to a known chat model: one that an enabled connection lists (not missing) with kind chat;
- it must not be the model itself;
- it must not have a fallback of its own (one step only);
- the model must not be the fallback of another model, since that would make a chain of two steps. The admin API lists `fallback_of` for each model, so the console can offer only valid choices.

The fallback is checked when it changes, so saving other fields of a model does not depend on the current state of its fallback.

`llm_models.fallback_after_s` is null (default 60) or 5 to 600.

A reference resolves to one stored row, the first enabled one in connection order (the routing choice). A model's settings are the settings of that row.

**Alternatives considered**
- *A policy map role → fallback, next to policy.llm.models:* Groups would each have to repeat it. policy.llm.models also merges as a whole map, which add-llm-connections found error-prone. The fallback is a property of the model's availability, not of a Group.

### D3. When the gateway falls back
For a chat request to model M that has a fallback F, the gateway tries M's backends as today, but with M's fallback wait as the first-byte limit (for non-streamed requests, the limit to the response headers). It switches to F when every attempt on M ended before its first byte with one of these:
- the fallback wait passed;
- TIMEOUT;
- MODEL_UNAVAILABLE (no healthy backend, 5xx or connection failure);
- an upstream 429, or all backends at capacity.

It does not switch on:
- CONTEXT_LENGTH or INVALID_REQUEST;
- QUOTA_EXCEEDED, or the user's own RATE_LIMIT (both are rejected before any backend is tried);
- any failure after the first byte.

**F must fit the request:**
- the estimated tokens fit F's context window;
- F supports tools if the request has tools;
- F supports images if the request has images.

Otherwise the original error stands.

**Backends of F.** A connection is one backend for all of its models, so a 503 from M marks the connection unhealthy, and F on the same connection would find no healthy backend. The fallback is a last resort, so when no backend of F is healthy the gateway still tries F's backends that are marked unhealthy, in connection order and not yet tried. Health probes restore the mark as before.

**Request to F:**
- The same body with `model` replaced and F's reasoning effort applied (D1). A null effort on F removes M's effort from the body.
- F gets the normal first-byte limit, 180 s.
- The stream to the client continues: keep-alives went out while M was tried, and F's bytes follow.
- Budget and rate accounting are the user's chat budget, as before.

**Alternatives considered**
- *Fall back only after the full 180 s first-byte limit:* users would wait 3 minutes before the fallback helps.
- *Race M and F in parallel:* this doubles load on an upstream that is already overloaded.

### D4. Cooldown
When a request falls back for a reason other than F not fitting, the gateway records "M cooling down until now + `SKOWORK_LLM_FALLBACK_COOLDOWN_MS`" (default 120,000) in memory. During the cooldown, new requests for M that F fits go straight to F, without the fallback wait. A request that F does not fit still tries M.

The first request after the cooldown tries M again. If that falls back too, the cooldown restarts.

**Alternatives considered**
- *No cooldown:* every request waits the fallback wait (60 s) during an outage.
- *A health probe of M:* this adds load and a second code path, while the next real request already probes.

### D5. Audit
`llm_usage` gets two columns:
- `answered_by`: the model id that produced the response;
- `fallback`: null, `timeout`, `unavailable`, `capacity` or `cooldown`.

`upstream_model` keeps the mapped model, so reports per role stay comparable.

## Risks / Trade-offs

- **[Risk] The fallback model answers worse, and users don't know.** → **Mitigation:** fallback needs an explicit choice by the administrator, and the audit shows how often it happened. Telling the client is a possible follow-up, out of scope here.
- **[Risk] A Thread that moves to F loses its prefix-cache reuse on M, so the first request on F prefills fully.** → **Mitigation:** this is accepted. F only answers while M is failing, and F's own cache warms over the following Turns.
- **[Risk] The upstream changes the efforts it accepts after a value was saved, and every request then fails with 400.** → **Mitigation:** the 400 reaches the client as INVALID_REQUEST, as today, and the audit shows it. The administrator can re-check by saving, which runs the probe. The Connections view's "Test" also probes the stored effort of every chat model.
- **[Risk] The cooldown is per gateway process.** → **Mitigation:** the control plane is one process (config.yaml). After a restart, the first request simply tries M again.
- **[Trade-off] A fallback wait shorter than a legitimate long prefill sends that request to F needlessly.** The primary prefilled 128k tokens in 9 s (add-agent-runtime D0), so 60 s by default leaves a wide margin. Administrators can raise the wait per model.

## Acceptance

**Automated (2026-11-10).**
- `npm --prefix control-plane run check` passes.
- `npm --prefix control-plane test`: 276 of 276 pass (262 before, 14 new in test/model-fallback.test.ts).
- The admin console builds.

**Task 4.1, real organization LLM API (2026-11-10, `control-plane/scripts/accept-fallback.ts`).** The script runs an in-process control plane on an in-memory database, with a locally signed test admin and the LLM connection from `.env`.

| Check | Result |
|---|---|
| Connection Test | ok, no effort failures |
| Primary, effort unset | 205 reasoning tokens, 2.1 s, 13:54 |
| Primary, effort `low` (probe accepted) | 43 reasoning tokens, 0.77 s, 13:54 |
| Secondary, effort `high` | refused: 400 INVALID_REQUEST, field `reasoning_effort`, reason `rejected`; the message quotes the upstream's "Supported types are xhigh (default), medium, and low", with the address removed |
| Model list | primary shows fallback `skoda-llm-secondary` with 5 s; secondary shows `fallback_of: primary/skoda-llm-primary` |
| Long request (random words, 180,904 prompt tokens, streamed) | answered by the secondary after the 5 s wait; 3 keep-alives, no error event, 48.9 s in total; audit `answered_by` = secondary, `fallback` = `timeout`, `upstream_model` = primary |
| Short request right after | sent straight to the secondary; audit `fallback` = `cooldown` |

The gateway estimated the long request at about 52,000 tokens, while the upstream counted 180,904: random hex words tokenize far worse than the character estimate assumes. The secondary's real window took it, so this did not affect the check. The fit check (D3) uses the same estimate as the context-length check, so it can let through a request that F then rejects with its own error, as Fallback Request allows.

**Task 3.1 browser check.** Pending: it needs a platform-admin sign-in through the identity provider in the admin console.
