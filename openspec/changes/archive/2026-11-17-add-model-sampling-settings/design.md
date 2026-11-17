# Design

## Decisions

### D1: Defaults, not overrides

Unlike `reasoning_effort`, which replaces the client's value, stored sampling values fill only fields the client's request lacks. Client features that set a deliberate value (for example a low temperature for a classifier) keep it; the agent loop sends none and so gets the vendor's recommendation.

### D2: Storage

One nullable JSON column `sampling` on `llm_models`, holding only the stored keys. It is returned with the other model settings and edited through the same model settings endpoint. Discovery does not touch it.

### D3: Validation only

The values are range-checked, not probed upstream: OpenAI-compatible servers ignore or accept these fields, and a one-token probe would not show whether a sampling value helps.

### D4: The settings applied in the dev stack (2026-11-17)

`skoda-llm-secondary`: temperature 1.0, top_p 0.95, top_k 20, min_p 0.0, presence_penalty 0.0, repetition_penalty 1.0 (the vendor's thinking-mode values). Separately, its capabilities were corrected to the served context of 262,144 tokens with 131,072 output tokens (one `max_tokens` covers reasoning and answer), replacing the code defaults of 65,536 and 4,096.

## Evaluation (2026-11-17)

Team expenses Local app, re-run with these settings on the dev gateway:

- The first reply reasoned for 29,264 tokens and created the app. The second reply wrote `server.js` and `public/index.html` in 23,136 tokens over 15 minutes. No reply reached the output limit.
- The app works in the browser:
  - 12 seeded expenses for 4 people over 3 months.
  - Add, edit and delete, with the change kept after a reload.
  - Filters by person, category and month.
  - Totals per category and per person, and bars in proportion to the category totals.
  - CSV export of the filtered list.
- The harness stopped after its 25-minute limit while the model wrote its closing reply. The limit is now 45 minutes.

The run found two defects outside this change:

- **Missing text in the written files.** Short spans were missing in about ten places, for example `padStart0')` for `padStart(2, '0')`. The model found and repaired them with further edits.
  - The cause is not established. The gateway writes its keep-alive comment on a timer, which can split an upstream event in the middle of a line if the upstream stalls there. The client ignores data it cannot parse and does not report it.
- **Parallel edits to the same file.** Two of them failed with a file-access error. A third reported success, but its change was lost.
