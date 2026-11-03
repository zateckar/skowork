# Proposal

## Why

Using the desktop app on 2026-10-31, the product owner could switch knowledge Sources on and off but could not import a document, look into the knowledge base, correct it, or decide what to share and what to keep private. The knowledge graph today is local-only, and sharing was a non-goal of add-knowledge-graph. The product owner decided that knowledge stays private by default and that a user can share chosen items with a Group through the control plane, where colleagues' agents can search them.

## What Changes

- Desktop app: a Knowledge view to import documents and folders (system file and folder pickers), write notes, search and browse the knowledge base, open an item's text and the entities and facts derived from it, edit or delete facts, rename entities, and forget or exclude items.
- A new Source kind `notes` for knowledge the user writes in the Knowledge view.
- Sharing: the user can share a document, a folder or a note with Groups they belong to, within policy.knowledge.sharing.groups. Shared items are published to a team knowledge space in the control plane. The owner sees what is shared with whom and can unshare at any time. Everything else stays private and local.
- knowledge.search also returns passages shared with the user's Groups, labelled with their owner and Group, as untrusted data.
- Control plane: a team knowledge service that stores shared items, embeds them through the gateway, answers searches for members of the target Groups, and lets platform-admins see and remove shared items.

Out of scope:
- Sharing mail, calendar, meeting or Thread knowledge, entities or facts. Only documents, folders and notes can be shared.
- Sharing with individual users (Groups only) and editing other users' items.
- Indexing network shares, SharePoint or OneDrive cloud-only files (unchanged non-goal).

## Capabilities

### New Capabilities
- `control-plane/team-knowledge`: storage, search and administration of knowledge items users share with Groups.

### Modified Capabilities
- `client/knowledge-graph`: Local-Only Knowledge Store (modified), File Ingestion Scope (modified: single documents), Knowledge Notes, Knowledge View, Knowledge Curation, Sharing Knowledge Items, Team Knowledge In Search.
- `control-plane/admin-console`: Team Knowledge Administration.

## Impact

- **Roadmap:** 16, after add-llm-connections. Depends on add-knowledge-graph, add-policy-and-sandbox (policy keys), add-agent-runtime (gateway) and add-telemetry-and-admin (console), and is archived after them.
- **Glossary:** Source kinds gain `notes`; `openspec/config.yaml` is updated.
- **Supersedes:** the add-knowledge-graph non-goal "cross-user knowledge sharing", for documents, folders and notes only.
- **Data protection:** shared content leaves the laptop by an explicit user action and is visible to the target Groups. Production rollout of sharing follows the organization-level approval process like other features; it is controlled by the ordinary policy key policy.knowledge.sharing.groups, which defaults to no Groups.
