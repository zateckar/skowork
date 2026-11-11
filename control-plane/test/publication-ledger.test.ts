import { beforeEach, afterEach, describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openDb, type Db } from '../src/db.ts';
import { ApiError } from '../src/http.ts';
import { commitPublicationRevision, type PublicationLedgerRequest } from '../src/knowledge/publication-ledger.ts';

describe('publication revision ledger', () => {
  let db: Db;
  beforeEach(() => {
    db = openDb(':memory:');
    db.exec('CREATE TABLE test_public_content (issuer TEXT, subject TEXT, publication_id TEXT, entry_id TEXT, text TEXT)');
  });
  afterEach(() => db.close());
  const request = (overrides: Partial<PublicationLedgerRequest> = {}): PublicationLedgerRequest => ({
    issuer: 'https://idp.example', subject: 'subject-1', publicationId: 'p1', operationId: 'op1',
    expectedRevision: 0, action: 'publish', requestEncoding: '{"entries":["one","two"]}', ...overrides,
  });
  const commit = (req: PublicationLedgerRequest, apply: (revision: number) => void = () => {}, authorize = () => {}) =>
    commitPublicationRevision(db, req, authorize, apply);
  const expectConflict = (fn: () => unknown) => assert.throws(fn, (err: unknown) => err instanceof ApiError && err.status === 409);

  it('fences competing writes and retries an identical operation without applying it twice', () => {
    let applied = 0;
    const first = request();
    assert.equal(commit(first, () => applied++).committedRevision, 1);
    const retry = commit(first, () => applied++);
    assert.deepEqual(retry, { committedRevision: 1, currentRevision: 1, replayed: true, adminBlocked: false });
    assert.equal(applied, 1);
    expectConflict(() => commit(request({ requestEncoding: 'different' })));
    expectConflict(() => commit(request({ operationId: 'op2' })));
    assert.equal(commit(request({ operationId: 'op2', expectedRevision: 1 })).currentRevision, 2);
    const afterUpdate = commit(first);
    assert.equal(afterUpdate.committedRevision, 1);
    assert.equal(afterUpdate.currentRevision, 2);
    assert.equal(afterUpdate.replayed, true);
  });

  it('rolls back content and ledger together on failure and rechecks authorization on retries', () => {
    const req = request();
    assert.throws(() => commit(req, () => {
      db.prepare('INSERT INTO test_public_content VALUES (?, ?, ?, ?, ?)').run(req.issuer, req.subject, req.publicationId, 'e', 'secret');
      throw new Error('simulated storage failure');
    }));
    assert.equal((db.prepare('SELECT count(*) AS n FROM test_public_content').get() as { n: number }).n, 0);
    assert.equal((db.prepare('SELECT count(*) AS n FROM knowledge_publication_ledger').get() as { n: number }).n, 0);
    commit(req);
    assert.throws(() => commit(req, () => assert.fail('replay applied'), () => { throw new ApiError(403, 'FORBIDDEN', 'revoked'); }),
      (err: unknown) => err instanceof ApiError && err.status === 403);
  });

  it('preserves unaffected entries, fences old writes and retains a content-free removal tombstone', () => {
    const first = request();
    commit(first, () => {
      const insert = db.prepare('INSERT INTO test_public_content VALUES (?, ?, ?, ?, ?)');
      insert.run(first.issuer, first.subject, first.publicationId, 'one', 'One');
      insert.run(first.issuer, first.subject, first.publicationId, 'two', 'Two');
    });
    commit(request({ action: 'withdraw', operationId: 'remove-one', expectedRevision: 1, requestEncoding: 'one' }), () => {
      db.prepare('DELETE FROM test_public_content WHERE entry_id = ?').run('one');
    });
    assert.deepEqual(db.prepare('SELECT entry_id FROM test_public_content').all().map((r) => r.entry_id), ['two']);
    expectConflict(() => commit(request({ operationId: 'delayed', expectedRevision: 1 })));
    commit(request({ action: 'withdraw', operationId: 'remove-all', expectedRevision: 2, requestEncoding: 'all' }), () => {
      db.exec('DELETE FROM test_public_content');
    });
    assert.equal((db.prepare('SELECT count(*) AS n FROM test_public_content').get() as { n: number }).n, 0);
    assert.equal((db.prepare('SELECT revision FROM knowledge_publication_ledger').get() as { revision: number }).revision, 3);
    expectConflict(() => commit(request({ operationId: 'old-create' })));
    assert.equal(commit(request({ operationId: 'confirmed-again', expectedRevision: 3 })).currentRevision, 4);
  });

  it('keeps an administrative removal blocked until an explicit resolution', () => {
    commit(request());
    commit(request({ operationId: 'admin-removal', expectedRevision: 1, action: 'admin-remove', requestEncoding: 'reason' }));
    expectConflict(() => commit(request({ operationId: 'recreate', expectedRevision: 2 })));
    assert.equal(commit(request({ operationId: 'withdraw', expectedRevision: 2, action: 'withdraw' })).adminBlocked, true);
    assert.equal(commit(request({ operationId: 'resolve', expectedRevision: 3, action: 'admin-resolve' })).adminBlocked, false);
    assert.equal(commit(request({ operationId: 'fresh-publish', expectedRevision: 4 })).currentRevision, 5);
    expectConflict(() => commit(request({ operationId: 'resolve-again', expectedRevision: 5, action: 'admin-resolve' })));
  });

  it('separates owners by verified issuer and subject, independent of usernames', () => {
    commit(request());
    assert.equal(commit(request({ subject: 'subject-2' })).currentRevision, 1);
    assert.equal(commit(request({ issuer: 'https://other-idp.example' })).currentRevision, 1);
    assert.equal((db.prepare('SELECT count(*) AS n FROM knowledge_publication_ledger').get() as { n: number }).n, 3);
  });

  it('retains barriers and idempotency across database reopen and a second connection', () => {
    const dir = mkdtempSync(join(tmpdir(), 'skowork-publication-ledger-'));
    const path = join(dir, 'control.db');
    let first: Db | undefined;
    let second: Db | undefined;
    try {
      first = openDb(path);
      const original = request();
      commitPublicationRevision(first, original, () => {}, () => {});
      second = openDb(path);
      commitPublicationRevision(second, request({ operationId: 'withdraw', expectedRevision: 1, action: 'withdraw' }), () => {}, () => {});
      expectConflict(() => commitPublicationRevision(first!, request({ operationId: 'stale', expectedRevision: 1 }), () => {}, () => {}));
      first.close(); first = undefined;
      second.close(); second = undefined;
      first = openDb(path);
      const replay = commitPublicationRevision(first, original, () => {}, () => assert.fail('old content restored'));
      assert.equal(replay.committedRevision, 1);
      assert.equal(replay.currentRevision, 2);
      assert.equal(replay.replayed, true);
    } finally {
      first?.close(); second?.close();
      rmSync(dir, { recursive: true, force: true });
    }
  });
});
