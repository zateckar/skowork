// Revision and removal fencing for publications (design D9). The publication service supplies current authorization,
// quotas and transactional content cleanup through the callbacks of commitPublicationRevision.
import { createHash } from 'node:crypto';
import { type Db, tx } from '../db.ts';
import { ApiError } from '../http.ts';

export { publicationLedgerSql } from './publication-schema.ts';

export type PublicationAction = 'publish' | 'withdraw' | 'admin-remove' | 'admin-resolve';
export interface PublicationLedgerRequest {
  issuer: string;
  subject: string;
  publicationId: string;
  operationId: string;
  expectedRevision: number;
  action: PublicationAction;
  // The server's exact validated request encoding, never a client-provided digest.
  requestEncoding: string;
}
export interface PublicationLedgerResult {
  committedRevision: number;
  currentRevision: number;
  replayed: boolean;
  adminBlocked: boolean;
}

const invalidId = /[^A-Za-z0-9._:-]/;
const validId = (value: string) => value.length > 0 && value.length <= 200 && !invalidId.test(value);
function conflict(message: string): never { throw new ApiError(409, 'CONFLICT', message); }

/** The opaque, stable key readers and administrators use for a publication; it does not reveal the owner's identity. */
export function publicationKey(issuer: string, subject: string, publicationId: string): string {
  return createHash('sha256').update(JSON.stringify([issuer, subject, publicationId]), 'utf8').digest('base64url').slice(0, 32);
}

/**
 * Both callbacks run synchronously inside the same write transaction. authorize must recheck
 * current authority, including on retries. apply must enforce quota and mutate all associated
 * content/search tables atomically. It is never called for an idempotent retry.
 */
export function commitPublicationRevision(
  db: Db, req: PublicationLedgerRequest, authorize: () => void, apply: (revision: number) => void,
): PublicationLedgerResult {
  if (!req.issuer || !req.subject || !validId(req.publicationId) || !validId(req.operationId) ||
      !Number.isSafeInteger(req.expectedRevision) || req.expectedRevision < 0 ||
      !['publish', 'withdraw', 'admin-remove', 'admin-resolve'].includes(req.action))
    throw new ApiError(400, 'INVALID_REQUEST', 'Invalid publication revision request.');
  const digest = createHash('sha256').update(JSON.stringify([req.action, req.expectedRevision, req.requestEncoding]), 'utf8').digest('hex');
  return tx(db, () => {
    authorize();
    const key = [req.issuer, req.subject, req.publicationId] as const;
    const row = db.prepare('SELECT revision, admin_blocked FROM knowledge_publication_ledger WHERE issuer = ? AND subject = ? AND publication_id = ?')
      .get(...key) as { revision: number; admin_blocked: number } | undefined;
    const current = row?.revision ?? 0;
    const blocked = !!row?.admin_blocked;
    const operation = db.prepare('SELECT request_hash, revision FROM knowledge_publication_operations WHERE issuer = ? AND subject = ? AND publication_id = ? AND operation_id = ?')
      .get(...key, req.operationId) as { request_hash: string; revision: number } | undefined;
    if (operation) {
      if (operation.request_hash !== digest) return conflict('This operation id was already used for a different request.');
      return { committedRevision: operation.revision, currentRevision: current, replayed: true, adminBlocked: blocked };
    }
    if (req.expectedRevision !== current) return conflict('The publication changed; review it again.');
    if (blocked && req.action === 'publish') return conflict('An administrator removed this publication.');
    if (req.action === 'admin-resolve' && !blocked) return conflict('This publication is not administratively blocked.');
    if (current >= Number.MAX_SAFE_INTEGER) return conflict('The publication revision limit was reached.');
    const next = current + 1;
    const nextBlocked = req.action === 'admin-remove' || (blocked && req.action !== 'admin-resolve');
    db.prepare(`INSERT INTO knowledge_publication_ledger (issuer, subject, publication_id, revision, admin_blocked, public_key)
      VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT (issuer, subject, publication_id)
      DO UPDATE SET revision = excluded.revision, admin_blocked = excluded.admin_blocked`).run(...key, next, nextBlocked ? 1 : 0, publicationKey(...key));
    apply(next);
    db.prepare(`INSERT INTO knowledge_publication_operations (issuer, subject, publication_id, operation_id, request_hash, revision)
      VALUES (?, ?, ?, ?, ?, ?)`).run(...key, req.operationId, digest, next);
    return { committedRevision: next, currentRevision: next, replayed: false, adminBlocked: nextBlocked };
  });
}
