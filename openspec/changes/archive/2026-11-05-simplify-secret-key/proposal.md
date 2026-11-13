# Proposal

## Why

On 2026-11-05 the production control plane would not start. SKOWORK_SECRET_KEY had to be the base64 encoding of exactly 32 bytes, which an admin cannot type by hand, and anything else crashed the process with a stack trace. The other secrets, such as SKOWORK_TELEMETRY_SECRET, accept any text.

## What Changes

- SKOWORK_SECRET_KEY accepts any passphrase of at least 16 characters. The control plane derives the 32-byte server key from it with scrypt. A value that is already the base64 encoding of 32 bytes is used as the key directly, so existing deployments keep working.
- A settings error at startup (a passphrase that is too short, or a key missing while LLM connections exist) ends the process with one `refusing to start: ...` line naming the setting, not a stack trace.
- The README and `deploy/.env.example` describe the passphrase and no longer require openssl.

## Out of scope

- Generating the key automatically. The key stays in the deployment's secrets, apart from the database volume (add-llm-connections D1).
- Re-encrypting stored credentials under a new key.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `control-plane/llm-gateway`: Server Key From A Passphrase (added).

## Impact

- **Roadmap:** 30 [Hardening]. Follows add-llm-connections (D1, credential encryption) and improve-deployment-guidance.
- **Code:** control-plane/src/gateway/connections.ts (key derivation), control-plane/src/main.ts (startup errors).
- **Tests:** control-plane/test/connections.test.ts.
- **Data:** none. Credentials stored under a base64 key stay readable.
