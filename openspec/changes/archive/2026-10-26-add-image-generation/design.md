# Design

## Context

- The organization LLM API lists five models: `acme-llm-primary`, `acme-llm-secondary`, `acme-llm-embedding`, `acme-llm-reranker` and `acme-llm-image`. The first four are mapped to roles by add-agent-runtime. The fifth was unused.
- control-plane/llm-gateway is the only path to models (project constraint). The client never calls the upstream directly.
- Thread attachments already exist (glossary: a Thread holds attachments). add-work-inbox stores them in `app.db` and covers them by forget.

## Validation Results (2026-10-23)

One request from a development workstation to `POST {OPENAI_BASE_URL}/images/generations` with `{"model": "acme-llm-image", "prompt": "...", "n": 1}`:
- 200 after 21.1 s;
- the response has `created`, `background`, `data`, `output_format` (png), `quality`, `size` and `usage`;
- `data[0]` carries `b64_json` (about 4.2 MB of base64, a 1024×1024 PNG of about 3.1 MB), an empty `revised_prompt`, and no `url`;
- `usage` reports zero tokens.

## Goals / Non-Goals

**Goals:**
- The agent can make an image when asked, and the user sees it in the Thread.
- Image use is bounded per user, separately from the token budgets.

**Non-Goals:**
- Editing existing images.
- Letting the model look at what it generated.

## Decisions

### D1. A separate `image` role
- **Choice:** a new role `image` in policy.llm.models, default `acme-llm-image`. The gateway accepts it only on the image generation endpoint.
- **Why:** the role system already provides per-Group allowlisting and removal; a Group that must not generate images removes the role.
- **Alternatives considered:** reusing `vision` (that role means image input for chat, a different endpoint and model); a tool-only policy switch without a role (would bypass the gateway's allowlist).

### D2. Budget by image count
- **Choice:** policy.llm.budget.images_daily, the number of generated images per user per day (default 50). It is checked before forwarding, and one generated image counts as 1.
- **Why:** the upstream reports zero tokens for image requests, so token budgets cannot bound them. Image generation is the most expensive request per call (about 20 s of GPU time).
- **Alternative considered:** counting a fixed token equivalent per image against the chat budget. Rejected because it hides image use in a number users cannot interpret.

### D3. Tool shape and risk class
- **Choice:** `image.generate` with `prompt` (required, at most 4,000 characters), `size` (`1024x1024` default, `1024x1536`, `1536x1024`) and `count` (1 to 4, default 1). Risk class read.
- **Why read:** the prompt goes only to the organization gateway, like every chat request, and the result is stored inside Workbench as a Thread attachment. Nothing on the user's disk or outside the organization changes. Saving an image elsewhere is the user's own action in the UI, or a files.write call by the agent with its own risk class.
- **Alternatives considered:** write-local (would ask for approval on every image, with no protective value); returning the image to the model as an image tool result (out of scope; costs a vision request per image).

### D4. Storage and result
- **Choice:** the client decodes `b64_json`, checks that it is a PNG, JPEG or WebP image of at most 20 MB, and stores it as a Thread attachment with provenance source kind `tool` and the tool id. The tool result text names the attachment id, size and pixel dimensions. The Thread feed shows the attachment inline as a thumbnail that opens full size, with Save as.
- **Why:** attachments are already covered by forget and by the Thread's lifetime. Image bytes do not enter the model context, which keeps client/agent-runtime Image Tool Results unchanged: that requirement covers images sent to the model, and these are not.
- **Alternative considered:** saving to the user's Pictures folder. Rejected because it writes outside Workbench without asking.

### D5. Timeouts
- **Choice:** the gateway allows 120 s for an image request, and the client's tool call allows 150 s. Cancelling the Run aborts the request.
- **Why:** the measured latency is about 21 s for one image; four images or a busy backend can take several times that.

## Implementation Notes

- Gateway (tasks 2.1–2.3): `POST /llm/v1/images/generations` accepts only the `image` role. It forwards only the prompt, `n` (1 to 4) and `size` (the three sizes of D3); any other field is dropped, and invalid values are refused with 400 INVALID_REQUEST before forwarding. The budget check is `images used today + n > policy.llm.budget.images_daily`, so a request that would cross the budget is refused whole. `llm_usage` has a new `images` column with the number of images returned, in its own bucket `image`; image rows carry zero tokens. At telemetry level content the audit keeps `{request: {prompt, n, size}}` and never the image data. The upstream timeout for images is 120 s (`WORKBENCH_LLM_IMAGE_TIMEOUT_MS`). The local real-API test generated one 1024×1024 PNG in 21.5 s.
- Client (tasks 3.1, 3.2): `ModelClient::generate_images` sends one request with the `image` role and waits up to 150 s for the answer. It does not retry: a request takes about 20 s per image, and a retry after a timeout would spend the image budget again. A tool returns generated files in `ToolOutput::files`. The runtime stores each one as a Thread attachment, with provenance source kind `tool` and the tool id as source id, and lists the files (id, name, type, size, pixels) in the tool result's `files`, which is where the feed finds them. To the model it appends one line per file with the attachment id, the pixel size and the byte size. The type and size checks read the PNG, JPEG or WebP header in portable code (`tools/image_info`), without decoding. All images of a request are checked before any is stored, so a request stores all of its images or none. A Run cancelled while the request is in flight stores nothing, even when the gateway answers anyway. `image.generate` is loaded lazily like the other non-core tools, so the agent finds it through `find_tools`.

- Feed (task 3.3): a tool result with image files shows the images under its tool card, so they stay visible while the card stays collapsed. Each image has a placeholder of the right aspect ratio (at most 360 px high) until it is loaded, then the picture, or an error line when the file cannot be read or decoded. The window reads the bytes once per image through the host RPC `threads.attachment`, which returns an attachment only to its own Thread and only when it is an image; decoding (WIC, at most 2048 px on the long side) runs off the window thread. Open shows the image at full size in a dialog; Save as uses the system save dialog with the stored file name and announces where the file went. The accessible name is "Generated image, W × H pixels". Covered by the ShellImages tests (UI Automation, open, save, one read per image, other-Thread refusal) and the `shell_generated_image` golden.
- End to end (task 4.1, run locally on 2026-10-24): Workbench.exe signed in to the dev stack, and a user message in a Thread asked for one square picture. The agent found `image.generate` through `find_tools` and called it once. The Run succeeded in about 80 s, of which the image request took most. The Thread feed, built by the window's own feed code, listed one 1024×1024 PNG (3.1 MB) under the tool card. `threads.attachment` returned the same bytes, and the gateway's `llm_usage` had exactly one `image` row for the Run, with one image and outcome ok. The test is `AppE2E.ImageRequestedInAThreadAppearsInTheFeed`; it reads the audit from the dev stack's database (`dev/.stack/workbench.db`, or `WORKBENCH_E2E_CP_DB`).

## Risks / Trade-offs

- [Generated images can contain inappropriate or misleading content] → The upstream model's own safety filtering applies; the image is shown only to the requesting user in their own Thread; usage is audited like every gateway request (prompt only at telemetry level content).
- [A Run loops and spends the image budget] → The daily image budget stops it with QUOTA_EXCEEDED; the tool's `count` is capped at 4.
- [Large attachments grow `app.db`] → 20 MB cap per image; storage quota of client/local-storage applies; forget removes them with the Thread.

## Open Questions

- Should a Group be able to lower the image size or count limits, beyond the daily budget? Not needed for the first version.
