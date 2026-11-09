import { afterEach, beforeEach, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { openDb, type Db } from '../src/db.ts';
import { parsePublicationInput } from '../src/knowledge/publication-input.ts';
import { commitPublicationRevision, publicationLedgerSql, type PublicationLedgerRequest } from '../src/knowledge/publication-ledger.ts';
import { publicationChunks, publicationContentSql, removePublicationContent, removePublicationEntry, replacePublicationContent } from '../src/knowledge/publication-content.ts';

describe('publication content transaction', () => {
  let db: Db;
  const owner = { issuer: 'idp', subject: 'person', publicationId: 'publication' };
  const request = (overrides: Partial<PublicationLedgerRequest> = {}): PublicationLedgerRequest => ({
    ...owner, operationId: 'op1', expectedRevision: 0, action: 'publish', requestEncoding: 'public-data', ...overrides,
  });
  const input = parsePublicationInput({
    expected_revision: 0, operation_id: 'op1', title: 'Approved title', groups: ['engineering', 'sales'],
    entries: [
      { id: 'entry1', text: 'First fact', source_kinds: ['files'], evidence: [{ id: 'evidence1', title: 'Doc', text: 'First excerpt' }] },
      { id: 'entry2', text: 'Second fact', source_kinds: ['notes'], evidence: [{ id: 'evidence2', title: 'Note', text: 'Second excerpt' }] },
    ],
  }, 20_000);
  beforeEach(() => { db = openDb(':memory:'); db.exec(publicationLedgerSql); db.exec(publicationContentSql); });
  afterEach(() => db.close());
  const run = (req: PublicationLedgerRequest, apply: () => void) => commitPublicationRevision(db, req, () => {}, apply);
  const count = (table: string) => Number((db.prepare(`SELECT count(*) AS n FROM ${table}`).get() as { n: number }).n);

  it('stores only confirmed title, audience, entries and approved excerpts', () => {
    run(request(), () => replacePublicationContent(db, owner, input));
    assert.equal((db.prepare('SELECT title FROM knowledge_publication_content').get() as { title: string }).title, 'Approved title');
    assert.deepEqual(db.prepare('SELECT group_name FROM knowledge_publication_groups ORDER BY position').all().map((r) => r.group_name), input.groups);
    assert.deepEqual(db.prepare('SELECT text FROM knowledge_publication_entries ORDER BY position').all().map((r) => r.text), ['First fact', 'Second fact']);
    assert.deepEqual(db.prepare('SELECT text FROM knowledge_publication_evidence ORDER BY entry_id').all().map((r) => r.text), ['First excerpt', 'Second excerpt']);
    assert.deepEqual(db.prepare('SELECT text FROM knowledge_publication_chunks ORDER BY entry_id').all().map((r) => r.text), ['First fact', 'Second fact']);
  });

  it('keeps supplementary Unicode characters intact at chunk boundaries', () => {
    const text = `${'a'.repeat(1023)}😀${'b'.repeat(3000)}😀end`;
    const chunks = publicationChunks(text);
    assert.ok(chunks.length > 1);
    assert.ok(chunks.every((chunk) => chunk.isWellFormed() && text.includes(chunk) && chunk.length <= 1024));
    assert.ok(chunks[0]!.startsWith('a'.repeat(100)));
    assert.ok(chunks.at(-1)!.endsWith('😀end'));
  });

  it('rolls back replacement content and revision when insertion fails', () => {
    run(request(), () => replacePublicationContent(db, owner, input));
    db.exec("CREATE TRIGGER fail_publication BEFORE INSERT ON knowledge_publication_evidence BEGIN SELECT RAISE(ABORT, 'simulated failure'); END");
    const next = { ...input, title: 'Replacement' };
    assert.throws(() => run(request({ operationId: 'op2', expectedRevision: 1, requestEncoding: 'new' }), () => replacePublicationContent(db, owner, next)));
    assert.equal((db.prepare('SELECT title FROM knowledge_publication_content').get() as { title: string }).title, 'Approved title');
    assert.equal(count('knowledge_publication_entries'), 2);
    assert.equal(count('knowledge_publication_evidence'), 2);
    assert.equal(count('knowledge_publication_chunks'), 2);
    assert.equal((db.prepare('SELECT revision FROM knowledge_publication_ledger').get() as { revision: number }).revision, 1);
    assert.equal(count('knowledge_publication_operations'), 1);
  });

  it('removes an entry and its excerpts, and then removes all content while retaining the fence', () => {
    run(request(), () => replacePublicationContent(db, owner, input));
    db.prepare('UPDATE knowledge_publication_chunks SET vector = ? WHERE entry_id = ?').run(Buffer.from([1, 2, 3]), 'entry1');
    assert.equal((db.prepare("SELECT count(*) AS n FROM knowledge_publication_chunks_fts WHERE knowledge_publication_chunks_fts MATCH 'First'").get() as { n: number }).n, 1);
    run(request({ operationId: 'remove1', action: 'withdraw', expectedRevision: 1, requestEncoding: 'entry1' }), () => {
      assert.equal(removePublicationEntry(db, owner, 'entry1'), true);
    });
    assert.deepEqual(db.prepare('SELECT entry_id FROM knowledge_publication_entries').all().map((r) => r.entry_id), ['entry2']);
    assert.deepEqual(db.prepare('SELECT evidence_id FROM knowledge_publication_evidence').all().map((r) => r.evidence_id), ['evidence2']);
    assert.equal(count('knowledge_publication_chunks'), 1);
    assert.equal((db.prepare("SELECT count(*) AS n FROM knowledge_publication_chunks_fts WHERE knowledge_publication_chunks_fts MATCH 'First'").get() as { n: number }).n, 0);
    assert.equal((db.prepare('SELECT count(*) AS n FROM knowledge_publication_chunks WHERE vector IS NOT NULL').get() as { n: number }).n, 0);
    run(request({ operationId: 'remove-all', action: 'withdraw', expectedRevision: 2, requestEncoding: 'all' }), () => removePublicationContent(db, owner));
    for (const table of ['knowledge_publication_content', 'knowledge_publication_groups', 'knowledge_publication_entries', 'knowledge_publication_evidence', 'knowledge_publication_chunks'])
      assert.equal(count(table), 0);
    assert.equal((db.prepare('SELECT revision FROM knowledge_publication_ledger').get() as { revision: number }).revision, 3);
    assert.equal(count('knowledge_publication_operations'), 3);
  });

  it('removes only the selected owner even when publication ids collide', () => {
    run(request(), () => replacePublicationContent(db, owner, input));
    const other = { ...owner, subject: 'different-subject' };
    run(request({ subject: other.subject }), () => replacePublicationContent(db, other, input));
    run(request({ operationId: 'remove', action: 'withdraw', expectedRevision: 1 }), () => removePublicationContent(db, owner));
    assert.equal(count('knowledge_publication_content'), 1);
    assert.equal((db.prepare('SELECT subject FROM knowledge_publication_content').get() as { subject: string }).subject, other.subject);
    assert.equal(count('knowledge_publication_entries'), 2);
  });

  it('removes the title and audience when the last entry is withdrawn', () => {
    run(request(), () => replacePublicationContent(db, owner, input));
    run(request({ operationId: 'remove1', action: 'withdraw', expectedRevision: 1 }), () => {
      assert.equal(removePublicationEntry(db, owner, 'entry1'), true);
    });
    run(request({ operationId: 'remove2', action: 'withdraw', expectedRevision: 2 }), () => {
      assert.equal(removePublicationEntry(db, owner, 'entry2'), true);
    });
    for (const table of ['knowledge_publication_content', 'knowledge_publication_groups', 'knowledge_publication_entries', 'knowledge_publication_evidence', 'knowledge_publication_chunks'])
      assert.equal(count(table), 0);
    assert.equal((db.prepare('SELECT revision FROM knowledge_publication_ledger').get() as { revision: number }).revision, 3);
  });
});
