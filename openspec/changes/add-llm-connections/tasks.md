## 1. Gateway

- [x] 1.1 Add the connection and model tables, credential encryption (D1) and the environment bootstrap (D2). Verify with tests: credential never in any response, encrypted at rest, bootstrap only when empty, production refuses to start without a key
- [x] 1.2 Implement discovery (D4), capabilities from stored settings and routing to connections with `<connection>/<id>` (D3). Verify with tests against a local fake OpenAI-compatible server: discovery, wrong credential, disabled connection, missing model, pinned connection
- [x] 1.3 Add the admin API for connections and models with audit records. Verify with tests for each operation and for the platform-admin-only access

## 2. Admin console

- [x] 2.1 Add the Connections view (add, edit, test, enable, disable, delete with the in-use check, models with capabilities). Verify in the browser against the real organization LLM API connection
- [x] 2.2 Replace the free-text model fields of the Models view with pickers of discovered models. Verify that picking a model and publishing changes the model a running client's Run uses

## 3. Acceptance

- [x] 3.1 Run the acceptance scenarios on the local stack and record them in design.md
- [x] 3.2 Run `openspec validate --all --strict`
