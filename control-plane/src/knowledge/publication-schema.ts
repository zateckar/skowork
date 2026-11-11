// Tables of team knowledge publications (improve-skowork-and-knowledge-control design D9), installed by a numbered
// control-plane migration. Kept free of imports so the migration list can include it without an import cycle.

/** The revision ledger and its idempotent operations. A ledger row is content-free and survives withdrawal. */
export const publicationLedgerSql = `
  CREATE TABLE knowledge_publication_ledger (
    issuer TEXT NOT NULL, subject TEXT NOT NULL, publication_id TEXT NOT NULL,
    revision INTEGER NOT NULL CHECK (revision >= 1), admin_blocked INTEGER NOT NULL CHECK (admin_blocked IN (0, 1)),
    public_key TEXT NOT NULL UNIQUE,
    PRIMARY KEY (issuer, subject, publication_id)
  );
  CREATE TABLE knowledge_publication_operations (
    issuer TEXT NOT NULL, subject TEXT NOT NULL, publication_id TEXT NOT NULL, operation_id TEXT NOT NULL,
    request_hash TEXT NOT NULL, revision INTEGER NOT NULL,
    PRIMARY KEY (issuer, subject, publication_id, operation_id),
    FOREIGN KEY (issuer, subject, publication_id) REFERENCES knowledge_publication_ledger (issuer, subject, publication_id)
  );
`;

/** Current public content only: title, audience, entries, approved excerpts and search chunks. */
export const publicationContentSql = `
  CREATE TABLE knowledge_publication_content (
    issuer TEXT NOT NULL, subject TEXT NOT NULL, publication_id TEXT NOT NULL, title TEXT NOT NULL,
    size INTEGER NOT NULL DEFAULT 0, embed_model TEXT, updated_at TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (issuer, subject, publication_id),
    FOREIGN KEY (issuer, subject, publication_id) REFERENCES knowledge_publication_ledger (issuer, subject, publication_id) ON DELETE CASCADE
  );
  CREATE TABLE knowledge_publication_groups (
    issuer TEXT NOT NULL, subject TEXT NOT NULL, publication_id TEXT NOT NULL,
    position INTEGER NOT NULL, group_name TEXT NOT NULL,
    PRIMARY KEY (issuer, subject, publication_id, group_name),
    UNIQUE (issuer, subject, publication_id, position),
    FOREIGN KEY (issuer, subject, publication_id) REFERENCES knowledge_publication_content (issuer, subject, publication_id) ON DELETE CASCADE
  );
  CREATE TABLE knowledge_publication_entries (
    issuer TEXT NOT NULL, subject TEXT NOT NULL, publication_id TEXT NOT NULL,
    entry_id TEXT NOT NULL, position INTEGER NOT NULL, text TEXT NOT NULL, source_kinds TEXT NOT NULL,
    PRIMARY KEY (issuer, subject, publication_id, entry_id),
    UNIQUE (issuer, subject, publication_id, position),
    FOREIGN KEY (issuer, subject, publication_id) REFERENCES knowledge_publication_content (issuer, subject, publication_id) ON DELETE CASCADE
  );
  CREATE TABLE knowledge_publication_evidence (
    issuer TEXT NOT NULL, subject TEXT NOT NULL, publication_id TEXT NOT NULL,
    entry_id TEXT NOT NULL, evidence_id TEXT NOT NULL, position INTEGER NOT NULL, title TEXT NOT NULL, text TEXT NOT NULL,
    PRIMARY KEY (issuer, subject, publication_id, evidence_id),
    UNIQUE (issuer, subject, publication_id, entry_id, position),
    FOREIGN KEY (issuer, subject, publication_id, entry_id)
      REFERENCES knowledge_publication_entries (issuer, subject, publication_id, entry_id) ON DELETE CASCADE
  );
  CREATE TABLE knowledge_publication_chunks (
    id INTEGER PRIMARY KEY,
    issuer TEXT NOT NULL, subject TEXT NOT NULL, publication_id TEXT NOT NULL, entry_id TEXT NOT NULL,
    position INTEGER NOT NULL, text TEXT NOT NULL, vector BLOB,
    UNIQUE (issuer, subject, publication_id, entry_id, position),
    FOREIGN KEY (issuer, subject, publication_id, entry_id)
      REFERENCES knowledge_publication_entries (issuer, subject, publication_id, entry_id) ON DELETE CASCADE
  );
  CREATE VIRTUAL TABLE knowledge_publication_chunks_fts USING fts5(text, content='knowledge_publication_chunks', content_rowid='id');
  CREATE TRIGGER knowledge_publication_chunks_ai AFTER INSERT ON knowledge_publication_chunks BEGIN
    INSERT INTO knowledge_publication_chunks_fts (rowid, text) VALUES (new.id, new.text);
  END;
  CREATE TRIGGER knowledge_publication_chunks_ad AFTER DELETE ON knowledge_publication_chunks BEGIN
    INSERT INTO knowledge_publication_chunks_fts (knowledge_publication_chunks_fts, rowid, text) VALUES ('delete', old.id, old.text);
  END;
`;

/** Who each owner was at their last authenticated request, and administrator removals the owner can see. */
export const publicationOwnerSql = `
  -- The owner's last authenticated identity: the mutable username (for Group membership and policy) and the identity
  -- provider roles with the expiry of the token they came from. Role-derived Groups are not current after that expiry.
  CREATE TABLE knowledge_publication_owners (
    issuer TEXT NOT NULL, subject TEXT NOT NULL, user_name TEXT NOT NULL, idp_roles TEXT NOT NULL,
    roles_expire_at INTEGER NOT NULL, updated_at TEXT NOT NULL,
    PRIMARY KEY (issuer, subject)
  );
  -- An administrator removal with its bounded reason; no publication content.
  CREATE TABLE knowledge_publication_removals (
    issuer TEXT NOT NULL, subject TEXT NOT NULL, publication_id TEXT NOT NULL,
    revision INTEGER NOT NULL, reason TEXT NOT NULL, removed_by TEXT NOT NULL, removed_at TEXT NOT NULL,
    PRIMARY KEY (issuer, subject, publication_id)
  );
`;

/** The whole schema, as the control-plane migration installs it. */
export const publicationSchemaSql = `${publicationLedgerSql}${publicationContentSql}${publicationOwnerSql}`;
