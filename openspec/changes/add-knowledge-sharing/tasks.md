## 1. Control plane

- [x] 1.1 Add the team knowledge store (items, chunks, FTS5, vectors), publish, unshare, search and admin removal with audit (D2). Verify with tests: Group membership and policy checks, size limit, search isolation between Groups, unshare deletes, revoked owner hidden
- [x] 1.2 Add policy.knowledge.sharing.groups and policy.knowledge.sources.notes to the policy schema. Verify with policy schema tests
- [x] 1.3 Add the Team knowledge view to the admin console. Verify in the browser: list by Group, remove with reason

## 2. Client

- [x] 2.1 Single-document import in the files Source and the notes Source. Verify with knowledge tests: a single pdf indexed alone, a note found by knowledge.search
- [x] 2.2 Fact edit and delete with tombstones, entity rename (D5). Verify with knowledge tests
- [x] 2.3 Sharing: share rules, publish and re-publish on change, unshare, refusal when policy permits no Group (D1, D3). Verify with tests against a local control plane
- [x] 2.4 Team results in knowledge.search with owner and Group provenance, and the offline notice. Verify with tests
- [x] 2.5 The Knowledge view (D4): import, notes, search, browse, item pane, curation, sharing. Verify with hidden-window UI tests

## 3. Acceptance

- [x] 3.1 On the local stack with the organization Keycloak user and a locally signed test user in another Group: import and share a document, find it from the other user's search, unshare, admin removal. Record the result in design.md
- [x] 3.2 Run `openspec validate --all --strict`
