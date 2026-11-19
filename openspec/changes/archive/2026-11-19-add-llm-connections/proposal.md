# Proposal

## Why

On 2026-10-31 the product owner tried to set the LLM for the desktop application in the admin console and found no such option. The upstream address and credential of the gateway are environment variables (control-plane/llm-gateway Organization LLM API Upstream), model capabilities are code defaults, and the Models view asks for upstream model ids as free text. An administrator cannot add or change an LLM endpoint, see which models it offers, or pick a model from a list without redeploying the control plane. The product owner decided that LLM connections are managed in the admin console.

## What Changes

- Add LLM connections: named OpenAI-compatible endpoints with a credential, managed by platform-admins in the admin console. The credential is encrypted at rest and never returned by any API.
- The gateway discovers each connection's models, and admins set the capabilities of each model (kind, context window, tool and image support, dimension).
- Role mappings (policy.llm.models) name a model of a connection; the Models view offers a picker of the discovered models.
- The environment variables OPENAI_BASE_URL and OPENAI_KEY become a bootstrap: when no connection exists, the control plane creates one named `primary` from them.

Out of scope:
- Per-user API keys or client-side model access (all traffic still goes through the gateway).
- Non-OpenAI-compatible APIs.
- Changes to budgets, rate limits or failover semantics.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `control-plane/llm-gateway`: Organization LLM API Upstream (modified), LLM Connections, Model Capabilities From Connections.
- `control-plane/admin-console`: LLM Connection Administration.

## Impact

- **Roadmap:** 15, after improve-usability. Depends on add-agent-runtime (llm-gateway) and add-telemetry-and-admin (admin-console), and is archived after them.
- **Code:** control-plane/src/gateway (connections store, discovery, routing), control-plane/src/config.ts (bootstrap), control-plane/admin (Connections view, model pickers).
- **Data:** a new table of connections and discovered models in the control-plane database; credentials encrypted with a server key from SKOWORK_SECRET_KEY (development: a generated key file in the data folder).
