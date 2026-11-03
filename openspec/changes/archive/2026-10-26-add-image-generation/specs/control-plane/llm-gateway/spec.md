# Spec Delta

## ADDED Requirements

### Requirement: Image Generation Endpoint
The gateway SHALL expose an OpenAI-compatible image generation endpoint that accepts only the `image` role as its model. It forwards the prompt, size and count to the upstream model mapped to `image` and returns the generated images as base64 data. It MUST refuse any other role on this endpoint, and the `image` role on every other endpoint, with 400 INVALID_REQUEST.

#### Scenario: Generate an image
- **WHEN** a user whose policy grants the `image` role requests one image
- **THEN** the gateway forwards the request to the upstream model mapped to `image` and returns the image as base64 data

#### Scenario: Image role on chat completions
- **WHEN** a client sends a chat completion request with model "image"
- **THEN** the gateway refuses it with 400 INVALID_REQUEST, and no backend call is made

### Requirement: Image Role Mapping
policy.llm.models SHALL also map the role `image`, with the default `image` → acme-llm-image. Like every role, a Group can remove it, and a user without it is refused with 403 MODEL_UNAVAILABLE and the reason "role not granted". The model list shows the `image` role with `kind` "image" to users who have it.

#### Scenario: Group without image generation
- **WHEN** a user's effective policy.llm.models has no `image` entry and the user requests an image
- **THEN** the gateway refuses with 403 MODEL_UNAVAILABLE and the reason "role not granted"

### Requirement: Image Budget
The gateway SHALL limit each user to policy.llm.budget.images_daily generated images per day (integer, default 50). It checks the budget before forwarding: a request whose image count would exceed the remaining budget is rejected with QUOTA_EXCEEDED and the reset time. Image requests MUST NOT count against the chat or embed token budgets.

#### Scenario: Image budget used up
- **WHEN** a user who has generated 50 images today requests another one under the default budget
- **THEN** the gateway rejects it with QUOTA_EXCEEDED and the time the budget resets, and no backend call is made

#### Scenario: Chat budget unaffected
- **WHEN** a user generates 10 images in a day
- **THEN** that user's chat token budget for the day is unchanged
