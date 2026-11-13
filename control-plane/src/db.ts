// SQLite storage through node:sqlite (design D10). Numbered migrations run at start, each in a transaction.
import { DatabaseSync } from 'node:sqlite';
import { mkdirSync } from 'node:fs';
import { dirname } from 'node:path';
import { publicationSchemaSql } from './knowledge/publication-schema.ts';

export type Db = DatabaseSync;

const migrations: string[] = [
  // 1: identity and fleet (add-client-foundation)
  `
  CREATE TABLE audit (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    at TEXT NOT NULL,
    category TEXT NOT NULL,
    action TEXT NOT NULL,
    outcome TEXT NOT NULL,
    subject TEXT,
    user_name TEXT,
    installation_id TEXT,
    source_ip TEXT,
    reason TEXT,
    details TEXT
  );
  CREATE INDEX audit_at ON audit(at);
  CREATE TABLE seen_sessions (sid TEXT PRIMARY KEY, first_seen TEXT NOT NULL);
  CREATE TABLE deny_list (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK (kind IN ('user', 'installation')),
    value TEXT NOT NULL,
    reason TEXT NOT NULL,
    created_by TEXT NOT NULL,
    created_at TEXT NOT NULL,
    UNIQUE (kind, value)
  );
  CREATE TABLE role_mappings (idp_role TEXT NOT NULL, skowork_group TEXT NOT NULL, PRIMARY KEY (idp_role, skowork_group));
  CREATE TABLE group_members (user_name TEXT NOT NULL, skowork_group TEXT NOT NULL, PRIMARY KEY (user_name, skowork_group));
  CREATE TABLE admin_roles (user_name TEXT NOT NULL, role TEXT NOT NULL, PRIMARY KEY (user_name, role));
  CREATE TABLE installations (
    id TEXT PRIMARY KEY,
    subject TEXT NOT NULL,
    user_name TEXT NOT NULL,
    groups TEXT NOT NULL DEFAULT '[]',
    machine_name TEXT NOT NULL DEFAULT '',
    version TEXT NOT NULL DEFAULT '',
    channel TEXT NOT NULL DEFAULT 'stable',
    commit_id TEXT NOT NULL DEFAULT '',
    os_build TEXT NOT NULL DEFAULT '',
    hardware_class TEXT NOT NULL DEFAULT '',
    policy_version TEXT NOT NULL DEFAULT '',
    status TEXT NOT NULL DEFAULT 'idle',
    source_ip TEXT,
    first_seen TEXT NOT NULL,
    last_seen TEXT NOT NULL,
    revoked_at TEXT
  );
  CREATE INDEX installations_user ON installations(user_name);
  CREATE TABLE min_versions (channel TEXT PRIMARY KEY, version TEXT NOT NULL, set_by TEXT NOT NULL, set_at TEXT NOT NULL);
  CREATE TABLE push_messages (
    installation_id TEXT NOT NULL,
    id INTEGER NOT NULL,
    type TEXT NOT NULL,
    target TEXT NOT NULL,
    payload TEXT NOT NULL,
    issued_at TEXT NOT NULL,
    expires_at TEXT NOT NULL,
    delivered_at TEXT,
    acked_at TEXT,
    PRIMARY KEY (installation_id, id)
  );
  CREATE TABLE push_sequences (installation_id TEXT PRIMARY KEY, last_id INTEGER NOT NULL);
  `,
  // 2: LLM gateway (add-agent-runtime)
  `
  CREATE TABLE llm_usage (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    at TEXT NOT NULL,
    subject TEXT NOT NULL,
    user_name TEXT NOT NULL,
    installation_id TEXT,
    endpoint TEXT NOT NULL,
    role TEXT,
    upstream_model TEXT,
    bucket TEXT,
    prompt_tokens INTEGER NOT NULL DEFAULT 0,
    completion_tokens INTEGER NOT NULL DEFAULT 0,
    reasoning_tokens INTEGER NOT NULL DEFAULT 0,
    total_tokens INTEGER NOT NULL DEFAULT 0,
    outcome TEXT NOT NULL,
    duration_ms INTEGER NOT NULL,
    content TEXT
  );
  CREATE INDEX llm_usage_subject ON llm_usage(subject, bucket, at);
  `,
  // 3: usage audit with the Run identifier and the user's Groups (control-plane/llm-gateway Usage Audit)
  `
  ALTER TABLE llm_usage ADD COLUMN run_id TEXT;
  ALTER TABLE llm_usage ADD COLUMN groups TEXT NOT NULL DEFAULT '[]';
  `,
  // 4: policy service (add-policy-and-sandbox design D7, D12, D14)
  `
  CREATE TABLE policy_groups (
    name TEXT PRIMARY KEY,
    description TEXT NOT NULL DEFAULT '',
    owners TEXT NOT NULL DEFAULT '[]',
    created_by TEXT NOT NULL,
    created_at TEXT NOT NULL
  );
  -- scope is 'group:<name>' or 'guardrail'; version 0 is the draft, published versions are 1, 2, ... and never change.
  CREATE TABLE policy_versions (
    scope TEXT NOT NULL,
    version INTEGER NOT NULL,
    priority INTEGER NOT NULL DEFAULT 0,
    content TEXT NOT NULL,
    created_by TEXT NOT NULL,
    created_at TEXT NOT NULL,
    reason TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (scope, version)
  );
  CREATE TABLE policy_scopes (
    scope TEXT PRIMARY KEY,
    current_version INTEGER,
    previous_version INTEGER,
    candidate_version INTEGER,
    rollout_kind TEXT CHECK (rollout_kind IN ('pilot', 'percent')),
    rollout_value TEXT,
    updated_at TEXT NOT NULL
  );
  CREATE TABLE user_policy_versions (
    user_name TEXT PRIMARY KEY,
    version INTEGER NOT NULL,
    content_hash TEXT NOT NULL,
    bundle TEXT NOT NULL,
    issued_at TEXT NOT NULL,
    expires_at TEXT NOT NULL
  );
  CREATE TABLE dispatch_rules (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    issuer_kind TEXT NOT NULL CHECK (issuer_kind IN ('user', 'role', 'group', 'group-owner', 'service-principal')),
    issuer TEXT NOT NULL,
    target_kind TEXT NOT NULL CHECK (target_kind IN ('user', 'group', 'any')),
    target TEXT NOT NULL DEFAULT '',
    created_by TEXT NOT NULL,
    created_at TEXT NOT NULL,
    UNIQUE (issuer_kind, issuer, target_kind, target)
  );
  CREATE TABLE service_signing_keys (
    kid TEXT PRIMARY KEY,
    purpose TEXT NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('current', 'next', 'retired')),
    private_jwk TEXT NOT NULL,
    public_jwk TEXT NOT NULL,
    created_at TEXT NOT NULL,
    retired_at TEXT
  );
  ALTER TABLE installations ADD COLUMN network_enforcement TEXT NOT NULL DEFAULT '';
  ALTER TABLE installations ADD COLUMN idp_roles TEXT NOT NULL DEFAULT '[]';
  `,
  // 5: MCP catalog (add-core-tools-and-mcp design D5, D6)
  `
  CREATE TABLE mcp_servers (
    id TEXT PRIMARY KEY,
    entry TEXT NOT NULL,
    enabled INTEGER NOT NULL DEFAULT 0,
    discovery_status TEXT NOT NULL CHECK (discovery_status IN ('none', 'ok', 'failed')),
    discovery_error TEXT,
    discovered_at TEXT,
    updated_by TEXT NOT NULL,
    updated_at TEXT NOT NULL
  );
  CREATE TABLE mcp_assignments (
    server_id TEXT NOT NULL REFERENCES mcp_servers (id) ON DELETE CASCADE,
    skowork_group TEXT NOT NULL,
    PRIMARY KEY (server_id, skowork_group)
  );
  -- status: baseline (first snapshot), pending (appeared later, awaits review), included, excluded.
  CREATE TABLE mcp_tools (
    server_id TEXT NOT NULL REFERENCES mcp_servers (id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    annotations TEXT NOT NULL DEFAULT '{}',
    status TEXT NOT NULL CHECK (status IN ('baseline', 'pending', 'included', 'excluded')),
    present INTEGER NOT NULL DEFAULT 1,
    first_seen TEXT NOT NULL,
    last_seen TEXT NOT NULL,
    decided_by TEXT,
    decided_at TEXT,
    PRIMARY KEY (server_id, name)
  );
  -- One row per change; the snapshot is the entry with its Groups and state (NULL after removal). Max id = catalog stamp.
  CREATE TABLE mcp_server_versions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    server_id TEXT NOT NULL,
    action TEXT NOT NULL,
    snapshot TEXT,
    changed_by TEXT NOT NULL,
    changed_at TEXT NOT NULL
  );
  CREATE TABLE mcp_packages (
    sha256 TEXT PRIMARY KEY,
    size INTEGER NOT NULL,
    name TEXT NOT NULL DEFAULT '',
    bytes BLOB NOT NULL,
    uploaded_by TEXT NOT NULL,
    uploaded_at TEXT NOT NULL
  );
  `,
  // add-image-generation: generated images per request, for the image budget (design D2).
  `
  ALTER TABLE llm_usage ADD COLUMN images INTEGER NOT NULL DEFAULT 0;
  `,
  // add-telemetry-and-admin: telemetry records, the forwarding buffer, pseudonyms and crash reports.
  `
  CREATE TABLE telemetry_records (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    received_at TEXT NOT NULL,
    at TEXT NOT NULL,
    user_name TEXT NOT NULL,
    installation_id TEXT NOT NULL,
    groups TEXT NOT NULL DEFAULT '[]',
    event TEXT NOT NULL,
    attrs TEXT NOT NULL DEFAULT '{}'
  );
  CREATE INDEX telemetry_records_received ON telemetry_records(received_at);
  CREATE INDEX telemetry_records_event ON telemetry_records(event, at);
  CREATE INDEX telemetry_records_user ON telemetry_records(user_name, at);
  CREATE TABLE telemetry_outbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at TEXT NOT NULL,
    payload BLOB NOT NULL
  );
  CREATE TABLE telemetry_pseudonyms (
    pseudonym TEXT PRIMARY KEY,
    kind TEXT NOT NULL,
    value TEXT NOT NULL,
    created_at TEXT NOT NULL
  );
  CREATE TABLE telemetry_crashes (
    id TEXT PRIMARY KEY,
    received_at TEXT NOT NULL,
    user_name TEXT NOT NULL,
    installation_id TEXT NOT NULL,
    process TEXT NOT NULL DEFAULT '',
    size INTEGER NOT NULL,
    iv BLOB NOT NULL,
    data BLOB NOT NULL
  );
  `,
  // add-telemetry-and-admin: the audit store is append-only (design D9); bundles record the scope versions they were
  // compiled from, for rollout adoption (design D11).
  `
  CREATE TRIGGER audit_no_update BEFORE UPDATE ON audit BEGIN SELECT RAISE(ABORT, 'audit events are append-only'); END;
  CREATE TRIGGER audit_no_delete BEFORE DELETE ON audit BEGIN SELECT RAISE(ABORT, 'audit events are append-only'); END;
  CREATE INDEX audit_category ON audit(category, id);
  ALTER TABLE user_policy_versions ADD COLUMN scopes TEXT NOT NULL DEFAULT '{}';
  `,
  // add-skills: the skill registry (design D6). A name belongs to its first publisher; a version never changes.
  `
  CREATE TABLE skills (name TEXT PRIMARY KEY, owner TEXT NOT NULL, created_at TEXT NOT NULL);
  CREATE TABLE skill_versions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL REFERENCES skills (name),
    version TEXT NOT NULL,
    scope TEXT NOT NULL CHECK (scope IN ('shared', 'group', 'org')),
    audience TEXT NOT NULL DEFAULT '[]',
    digest TEXT NOT NULL,
    size INTEGER NOT NULL,
    bytes BLOB NOT NULL,
    manifest TEXT NOT NULL,
    scan TEXT NOT NULL,
    state TEXT NOT NULL CHECK (state IN ('in-review', 'published', 'rejected', 'withdrawn', 'revoked')),
    author TEXT NOT NULL,
    source_url TEXT,
    licence TEXT,
    signature TEXT,
    review_comment TEXT,
    reviewed_by TEXT,
    reviewed_at TEXT,
    created_at TEXT NOT NULL,
    published_at TEXT,
    ended_at TEXT,
    revoke_reason TEXT,
    UNIQUE (name, version)
  );
  CREATE INDEX skill_versions_digest ON skill_versions(digest);
  CREATE INDEX skill_versions_state ON skill_versions(state, ended_at);
  CREATE TABLE skill_usage_weekly (
    name TEXT NOT NULL,
    week TEXT NOT NULL,
    installs INTEGER NOT NULL DEFAULT 0,
    runs INTEGER NOT NULL DEFAULT 0,
    failures INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (name, week)
  );
  `,
  // add-scheduled-and-remote-tasks: service principals, Remote tasks and their settings (design D5, D7, D8).
  `
  CREATE TABLE service_principals (
    client_id TEXT PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    apis TEXT NOT NULL DEFAULT '[]',
    registered_by TEXT NOT NULL,
    registered_at TEXT NOT NULL,
    retired_by TEXT,
    retired_at TEXT
  );
  CREATE TABLE task_issuer_limits (issuer TEXT PRIMARY KEY, per_hour INTEGER NOT NULL, updated_by TEXT NOT NULL, updated_at TEXT NOT NULL);
  CREATE TABLE task_callback_allowlist (prefix TEXT PRIMARY KEY, added_by TEXT NOT NULL, added_at TEXT NOT NULL);
  CREATE TABLE remote_task_parents (
    id TEXT PRIMARY KEY,
    issuer TEXT NOT NULL,
    target_kind TEXT NOT NULL CHECK (target_kind IN ('user', 'group')),
    target TEXT NOT NULL,
    created_at TEXT NOT NULL,
    expires_at TEXT NOT NULL
  );
  CREATE INDEX remote_task_parents_issuer ON remote_task_parents(issuer, created_at);
  CREATE TABLE remote_tasks (
    id TEXT PRIMARY KEY,
    parent_id TEXT NOT NULL REFERENCES remote_task_parents (id),
    issuer TEXT NOT NULL,
    issuer_name TEXT NOT NULL,
    user_name TEXT NOT NULL,
    prompt TEXT NOT NULL,
    skill TEXT,
    attachments TEXT NOT NULL DEFAULT '[]',
    callback_url TEXT,
    status TEXT NOT NULL CHECK (status IN ('queued', 'delivered', 'claimed', 'awaiting-acceptance', 'accepted', 'running',
                                           'succeeded', 'failed', 'declined', 'expired', 'cancelled')),
    claimed_by TEXT,
    result TEXT,
    created_at TEXT NOT NULL,
    expires_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
  );
  CREATE INDEX remote_tasks_parent ON remote_tasks(parent_id);
  CREATE INDEX remote_tasks_open ON remote_tasks(status, expires_at);
  CREATE TABLE remote_task_transitions (
    task_id TEXT NOT NULL REFERENCES remote_tasks (id),
    status TEXT NOT NULL,
    at TEXT NOT NULL,
    installation_id TEXT
  );
  CREATE INDEX remote_task_transitions_task ON remote_task_transitions(task_id);
  `,
  // LLM connections and their discovered models (add-llm-connections design D1)
  `
  CREATE TABLE llm_connections (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL UNIQUE,
    base_url TEXT NOT NULL,
    credential_enc TEXT,
    credential_hint TEXT,
    enabled INTEGER NOT NULL DEFAULT 1,
    max_concurrent INTEGER NOT NULL DEFAULT 0,
    position INTEGER NOT NULL DEFAULT 0,
    status TEXT NOT NULL DEFAULT 'unchecked',
    status_detail TEXT,
    checked_at TEXT
  );
  CREATE TABLE llm_models (
    connection_id INTEGER NOT NULL REFERENCES llm_connections (id) ON DELETE CASCADE,
    model_id TEXT NOT NULL,
    kind TEXT NOT NULL DEFAULT 'chat',
    context_window INTEGER NOT NULL DEFAULT 32768,
    max_output_tokens INTEGER,
    tools INTEGER NOT NULL DEFAULT 0,
    images INTEGER NOT NULL DEFAULT 0,
    reasoning INTEGER NOT NULL DEFAULT 0,
    dimension INTEGER,
    last_seen TEXT,
    missing INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (connection_id, model_id)
  );
  `,
  // Team knowledge: items shared with Groups, their chunks, full-text index and int8 vectors (add-knowledge-sharing D2)
  `
  CREATE TABLE team_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    owner TEXT NOT NULL,
    item_id TEXT NOT NULL,
    title TEXT NOT NULL,
    kind TEXT NOT NULL CHECK (kind IN ('files', 'notes')),
    content_hash TEXT NOT NULL,
    size INTEGER NOT NULL,
    embed_model TEXT,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    UNIQUE (owner, item_id)
  );
  CREATE TABLE team_item_groups (
    item INTEGER NOT NULL REFERENCES team_items (id) ON DELETE CASCADE,
    grp TEXT NOT NULL,
    PRIMARY KEY (item, grp)
  );
  CREATE INDEX team_item_groups_grp ON team_item_groups(grp);
  CREATE TABLE team_chunks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    item INTEGER NOT NULL REFERENCES team_items (id) ON DELETE CASCADE,
    seq INTEGER NOT NULL,
    text TEXT NOT NULL,
    vector BLOB
  );
  CREATE INDEX team_chunks_item ON team_chunks(item);
  CREATE VIRTUAL TABLE team_chunks_fts USING fts5(text, content='team_chunks', content_rowid='id');
  CREATE TRIGGER team_chunks_ai AFTER INSERT ON team_chunks BEGIN
    INSERT INTO team_chunks_fts (rowid, text) VALUES (new.id, new.text);
  END;
  CREATE TRIGGER team_chunks_ad AFTER DELETE ON team_chunks BEGIN
    INSERT INTO team_chunks_fts (team_chunks_fts, rowid, text) VALUES ('delete', old.id, old.text);
  END;
  CREATE TABLE team_owners (user_name TEXT PRIMARY KEY, groups TEXT NOT NULL, permitted TEXT NOT NULL, updated_at TEXT NOT NULL);
  `,
  // User messaging: profiles, Presence, Chats, members and messages (add-user-messaging design D1-D3)
  `
  CREATE TABLE user_profiles (
    user_name TEXT PRIMARY KEY COLLATE NOCASE,
    display_name TEXT NOT NULL,
    messaging INTEGER NOT NULL DEFAULT 1,
    busy INTEGER NOT NULL DEFAULT 0,
    last_seen TEXT NOT NULL
  );
  CREATE TABLE presence_reports (installation_id TEXT PRIMARY KEY, user_name TEXT NOT NULL COLLATE NOCASE, idle INTEGER NOT NULL, at TEXT NOT NULL);
  CREATE TABLE chats (
    id TEXT PRIMARY KEY,
    kind TEXT NOT NULL CHECK (kind IN ('direct', 'team')),
    name TEXT,
    pair_key TEXT UNIQUE,
    created_by TEXT NOT NULL,
    created_at TEXT NOT NULL,
    last_seq INTEGER NOT NULL DEFAULT 0
  );
  CREATE TABLE chat_members (
    chat TEXT NOT NULL REFERENCES chats (id) ON DELETE CASCADE,
    user_name TEXT NOT NULL COLLATE NOCASE,
    read_seq INTEGER NOT NULL DEFAULT 0,
    joined_at TEXT NOT NULL,
    PRIMARY KEY (chat, user_name)
  );
  CREATE INDEX chat_members_user ON chat_members(user_name);
  CREATE TABLE chat_messages (
    chat TEXT NOT NULL REFERENCES chats (id) ON DELETE CASCADE,
    seq INTEGER NOT NULL,
    id TEXT NOT NULL,
    sender TEXT NOT NULL,
    text TEXT NOT NULL,
    at TEXT NOT NULL,
    PRIMARY KEY (chat, seq),
    UNIQUE (chat, id)
  );
  CREATE INDEX chat_messages_at ON chat_messages(at);
  `,
  // use-idp-tokens-for-mcp D4: remote entries present the caller's identity-provider token; no control-plane token keys.
  `
  UPDATE mcp_servers SET entry = json_remove(json_set(entry, '$.auth', 'user-token'), '$.audience')
    WHERE json_extract(entry, '$.auth') = 'audience-token';
  DELETE FROM service_signing_keys WHERE purpose = 'mcp-token';
  `,
  // add-skills D6 Sole reviewer: a self-approval stays visible on the version.
  `
  ALTER TABLE skill_versions ADD COLUMN self_approved INTEGER NOT NULL DEFAULT 0;
  `,
  // Existing item shares: independently identify the uploaded bytes without trusting the client's hash.
  `
  ALTER TABLE team_items ADD COLUMN server_content_hash TEXT NOT NULL DEFAULT '';
  `,
  // fix-long-thread-model-calls D7: time to the first upstream byte and the prompt tokens served from the upstream cache;
  // NULL when not measured or not reported by the upstream (control-plane/llm-gateway Prompt Cache Audit).
  `
  ALTER TABLE llm_usage ADD COLUMN ttft_ms INTEGER;
  ALTER TABLE llm_usage ADD COLUMN cached_tokens INTEGER;
  `,
  // add-model-fallback-and-effort D1, D2, D5: per-model reasoning effort, fallback model and fallback wait (NULL = the
  // model's own default, no fallback, 60 s); the model that answered and the fallback reason of each chat request.
  `
  ALTER TABLE llm_models ADD COLUMN reasoning_effort TEXT;
  ALTER TABLE llm_models ADD COLUMN fallback_model TEXT;
  ALTER TABLE llm_models ADD COLUMN fallback_after_s INTEGER;
  ALTER TABLE llm_usage ADD COLUMN answered_by TEXT;
  ALTER TABLE llm_usage ADD COLUMN fallback TEXT;
  `,
  // add-api-connectors (control-plane/api-connectors): the API connector's uploaded operations (no raw document is
  // kept), the write-only api-key, and each user's personal access token per connector (design D6, D11). Connection
  // data without secrets stays in the catalog entry; secrets are encrypted with the server key.
  `
  CREATE TABLE api_operations (
    connector_id TEXT NOT NULL REFERENCES mcp_servers (id) ON DELETE CASCADE,
    op_key TEXT NOT NULL,
    method TEXT NOT NULL,
    path TEXT NOT NULL,
    operation_id TEXT,
    tool TEXT,
    description TEXT NOT NULL DEFAULT '',
    usable INTEGER NOT NULL DEFAULT 0,
    reason TEXT,
    input_schema TEXT,
    schema_bytes INTEGER NOT NULL DEFAULT 0,
    call_spec TEXT,
    status TEXT NOT NULL CHECK (status IN ('new', 'unselected', 'selected', 'gone')),
    risk TEXT NOT NULL DEFAULT 'read',
    risk_reason TEXT,
    decided_by TEXT,
    decided_at TEXT,
    PRIMARY KEY (connector_id, op_key)
  );
  CREATE TABLE api_documents (
    connector_id TEXT PRIMARY KEY REFERENCES mcp_servers (id) ON DELETE CASCADE,
    title TEXT NOT NULL,
    openapi TEXT NOT NULL,
    uploaded_by TEXT NOT NULL,
    uploaded_at TEXT NOT NULL
  );
  CREATE TABLE api_connector_secrets (
    connector_id TEXT PRIMARY KEY REFERENCES mcp_servers (id) ON DELETE CASCADE,
    key_enc TEXT NOT NULL,
    key_hint TEXT NOT NULL DEFAULT '',
    set_by TEXT NOT NULL,
    set_at TEXT NOT NULL
  );
  -- One row per user and connector; the token is encrypted and never returned. rejected_at is set on upstream 401/403.
  CREATE TABLE api_connector_tokens (
    connector_id TEXT NOT NULL REFERENCES mcp_servers (id) ON DELETE CASCADE,
    user_name TEXT NOT NULL COLLATE NOCASE,
    token_enc TEXT NOT NULL,
    set_at TEXT NOT NULL,
    rejected_at TEXT,
    PRIMARY KEY (connector_id, user_name)
  );
  `,
  // Team knowledge publications (improve-skowork-and-knowledge-control design D9).
  publicationSchemaSql,
  // Shared Item Ownership (improve-skowork-and-knowledge-control task 7.3): new and replaced item shares are owned by the
  // uploader's issuer and stable subject; rows without a subject stay legacy, keyed by their historical username. The
  // owner snapshot records whose snapshot it is, so a reassigned username cannot govern another subject's items.
  `
  ALTER TABLE team_items ADD COLUMN owner_issuer TEXT;
  ALTER TABLE team_items ADD COLUMN owner_subject TEXT;
  CREATE INDEX team_items_subject ON team_items(owner_issuer, owner_subject, item_id);
  CREATE INDEX team_items_item ON team_items(item_id);
  ALTER TABLE team_owners ADD COLUMN issuer TEXT;
  ALTER TABLE team_owners ADD COLUMN subject TEXT;
  `,
];

export function openDb(path: string): Db {
  if (path !== ':memory:') mkdirSync(dirname(path), { recursive: true });
  const db = new DatabaseSync(path);
  // synchronous NORMAL: in WAL mode a commit is durable against a process crash; a power loss can lose the last
  // commits but never corrupts the database. FULL would sync every commit, which the telemetry load cannot afford
  // (add-telemetry-and-admin design, ingest sizing).
  db.exec('PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA foreign_keys = ON; PRAGMA busy_timeout = 5000;');
  db.exec('CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL)');
  const row = db.prepare('SELECT version FROM schema_version').get() as { version: number } | undefined;
  let version = row?.version ?? 0;
  if (!row) db.prepare('INSERT INTO schema_version (version) VALUES (0)').run();
  for (; version < migrations.length; version++) {
    db.exec('BEGIN');
    try {
      db.exec(migrations[version]!);
      db.prepare('UPDATE schema_version SET version = ?').run(version + 1);
      db.exec('COMMIT');
    } catch (err) {
      db.exec('ROLLBACK');
      throw err;
    }
  }
  return db;
}

export const nowIso = (): string => new Date().toISOString();

/** Runs fn inside a transaction (node:sqlite is synchronous, so this is safe across awaits only if fn is sync). */
export function tx<T>(db: Db, fn: () => T): T {
  db.exec('BEGIN IMMEDIATE');
  try {
    const r = fn();
    db.exec('COMMIT');
    return r;
  } catch (err) {
    db.exec('ROLLBACK');
    throw err;
  }
}
