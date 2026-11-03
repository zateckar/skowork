## 1. Validation spike

- [x] 1.1 Call the organization LLM API image endpoint with `acme-llm-image` from a development workstation. Verify a 200 with a `b64_json` PNG and record latency and response shape in design.md (done 2026-10-23)

## 2. Control plane

- [x] 2.1 Add the `image` role to policy.llm.models (default `acme-llm-image`) and the key policy.llm.budget.images_daily (default 50) in the policy key registry, and regenerate the client's policy schema fixture. Verify with the policy tests and `npm run policy:schema`
- [x] 2.2 Implement the gateway's image generation endpoint for the `image` role: role checks, image budget before forwarding, 120 s upstream timeout, usage audit, error mapping. Verify with gateway unit tests against an in-test fake upstream (budget exhausted, role not granted, wrong role on the endpoint, timeout) and one local real-API test
- [x] 2.3 List the `image` role in the filtered model list with `kind` "image". Verify with a gateway test

## 3. Client

- [x] 3.1 Add image generation requests to the client's gateway client with the `image` role, size and count, and cancellation. Verify with unit tests against an in-test fake gateway
- [x] 3.2 Implement `image.generate` (risk read): argument validation, image type and size checks, storage as a Thread attachment with tool provenance, and the text result. Verify with win_tests (stored attachment, rejected non-image body, too-large body, cancellation)
- [x] 3.3 Show image attachments inline in the Thread feed of the native window (add-native-ui `FeedView`) with open full size and Save as. Verify with UI Automation and golden-image tests

## 4. End to end

- [x] 4.1 End-to-end test with the real model, local only: ask for an image in a Thread. Verify that the agent calls `image.generate`, the image appears in the feed, and the gateway audit has one image request
- [x] 4.2 Run `openspec validate --all --strict`
