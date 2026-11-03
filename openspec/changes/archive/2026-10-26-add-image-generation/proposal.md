# Proposal

## Why

Users ask Workbench for pictures (illustrations, diagrams, mock-ups), and the agent answers that it has no way to make one. The organization LLM API already offers an image generation model, `acme-llm-image`, through the standard OpenAI images endpoint (verified 2026-10-23, see design.md). The gateway has no role for it, and the agent has no tool that uses it.

## What Changes

- Add the model role `image` and map it by default to `acme-llm-image`.
- control-plane/llm-gateway exposes the OpenAI-compatible image generation endpoint for the `image` role, with its own per-user daily image budget, policy.llm.budget.images_daily.
- client/core-tools adds the tool `image.generate`, risk class read. It sends a text prompt to the gateway and stores each generated image as a Thread attachment.
- The Thread feed shows generated images inline, and the user can open or save them.
- Update the glossary in `openspec/config.yaml`: the Model role list gains `image`, and the Tool id examples gain `image.generate`.

Out of scope:
- Image editing, variations and inpainting (the images edit endpoint).
- Sending the generated image back to the model so it can inspect it. The model gets a text result only.
- Images in mail, documents or any other output besides the Thread.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `control-plane/llm-gateway`: image generation endpoint, `image` role mapping and the daily image budget.
- `client/model-access`: the `image` role and image generation requests.
- `client/core-tools`: the `image.generate` tool.
- `client/work-inbox`: generated images in the Thread feed.

## Impact

- **Roadmap:** inserted as 6b, after add-native-ui (6a). It depends on add-native-ui (the native window shows the images), add-agent-runtime (control-plane/llm-gateway, client/model-access, Thread attachments), add-work-inbox (feed) and add-core-tools-and-mcp (tool conventions). Those changes that are not yet archived must be archived before this one, because this change adds requirements to their capabilities.
- **Policy keys:** policy.llm.models gains the `image` key (owned by control-plane/llm-gateway); new policy.llm.budget.images_daily (owned by control-plane/llm-gateway).
- **Code:** `control-plane/src/gateway/*`, `control-plane/src/policy/keys.ts`, the client's gateway client and a new tool under `client/src/tools/`, Thread attachments in the runtime, and the Thread feed of the native window (add-native-ui).
- **Out of the gateway's token budgets:** image requests count only against the image budget, because the upstream reports no tokens for them.
