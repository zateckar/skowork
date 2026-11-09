// Current public content only. These operations must be called from the ledger's
// transaction, after current authorization and quota have been checked. This schema
// is not installed or served until search, withdrawal and compatibility are wired.
import type { Db } from '../db.ts';
import type { PublicationInput } from './publication-input.ts';

export const publicationContentSql = `
  CREATE TABLE knowledge_publication_content (
    issuer TEXT NOT NULL, subject TEXT NOT NULL, publication_id TEXT NOT NULL, title TEXT NOT NULL,
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

export interface PublicationOwnerKey { issuer: string; subject: string; publicationId: string }
const keyOf = (key: PublicationOwnerKey) => [key.issuer, key.subject, key.publicationId] as const;

/** Overlapping windows of reviewed entry text; never cut a UTF-16 surrogate pair. */
export function publicationChunks(text: string): string[] {
  const out: string[] = [];
  let start = 0;
  while (start < text.length) {
    let end = Math.min(start + 1024, text.length);
    if (end < text.length && text.charCodeAt(end) >= 0xdc00 && text.charCodeAt(end) <= 0xdfff) --end;
    out.push(text.slice(start, end));
    if (end === text.length) break;
    start = end - 128;
    if (start > 0 && text.charCodeAt(start) >= 0xdc00 && text.charCodeAt(start) <= 0xdfff) --start;
  }
  return out;
}

/** Replace only validated public data; any failed insert must roll back the surrounding revision transaction. */
export function replacePublicationContent(db: Db, owner: PublicationOwnerKey, input: PublicationInput): void {
  const key = keyOf(owner);
  removePublicationContent(db, owner);
  db.prepare('INSERT INTO knowledge_publication_content (issuer, subject, publication_id, title) VALUES (?, ?, ?, ?)').run(...key, input.title);
  const addGroup = db.prepare('INSERT INTO knowledge_publication_groups (issuer, subject, publication_id, position, group_name) VALUES (?, ?, ?, ?, ?)');
  const addEntry = db.prepare('INSERT INTO knowledge_publication_entries (issuer, subject, publication_id, entry_id, position, text, source_kinds) VALUES (?, ?, ?, ?, ?, ?, ?)');
  const addEvidence = db.prepare('INSERT INTO knowledge_publication_evidence (issuer, subject, publication_id, entry_id, evidence_id, position, title, text) VALUES (?, ?, ?, ?, ?, ?, ?, ?)');
  const addChunk = db.prepare('INSERT INTO knowledge_publication_chunks (issuer, subject, publication_id, entry_id, position, text) VALUES (?, ?, ?, ?, ?, ?)');
  input.groups.forEach((group, position) => addGroup.run(...key, position, group));
  input.entries.forEach((entry, position) => {
    addEntry.run(...key, entry.id, position, entry.text, JSON.stringify(entry.source_kinds));
    entry.evidence.forEach((excerpt, index) => addEvidence.run(...key, entry.id, excerpt.id, index, excerpt.title, excerpt.text));
    publicationChunks(entry.text).forEach((chunk, index) => addChunk.run(...key, entry.id, index, chunk));
  });
}

/** Remove the current title, audience, entries and evidence; the content-free ledger survives. */
export function removePublicationContent(db: Db, owner: PublicationOwnerKey): void {
  db.prepare('DELETE FROM knowledge_publication_content WHERE issuer = ? AND subject = ? AND publication_id = ?').run(...keyOf(owner));
}

/** Remove one entry and its excerpts, chunks and vectors; remove the title when no entries remain. */
export function removePublicationEntry(db: Db, owner: PublicationOwnerKey, entryId: string): boolean {
  const result = db.prepare('DELETE FROM knowledge_publication_entries WHERE issuer = ? AND subject = ? AND publication_id = ? AND entry_id = ?')
    .run(...keyOf(owner), entryId);
  const removed = Number(result.changes) === 1;
  if (removed) db.prepare(`DELETE FROM knowledge_publication_content WHERE issuer = ? AND subject = ? AND publication_id = ?
    AND NOT EXISTS (SELECT 1 FROM knowledge_publication_entries e WHERE e.issuer = knowledge_publication_content.issuer
      AND e.subject = knowledge_publication_content.subject AND e.publication_id = knowledge_publication_content.publication_id)`)
    .run(...keyOf(owner));
  return removed;
}
