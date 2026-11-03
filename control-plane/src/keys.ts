// Asymmetric signing keys of the control plane, one ring per purpose (policy bundles, exchanged tokens). Each ring has a
// current and a next key; rotation retires the current key, promotes the next one and creates a new next key. Retired
// keys stay published for a grace period so that everything they signed verifies until it expires.
import { randomUUID } from 'node:crypto';
import { exportJWK, generateKeyPair, importJWK, type CryptoKey, type JWK } from 'jose';
import type { Db } from './db.ts';
import { nowIso, tx } from './db.ts';

export class KeyRing {
  readonly #db: Db;
  readonly #purpose: string;
  readonly #graceS: () => number;
  #ready: Promise<void> | null = null;

  /** graceS: how long a retired key stays published (the longest validity of what it signed, plus a margin). */
  constructor(db: Db, purpose: string, graceS: () => number) {
    this.#db = db;
    this.#purpose = purpose;
    this.#graceS = graceS;
  }

  async #ensure(): Promise<void> {
    this.#ready ??= (async () => {
      const have = this.#db.prepare("SELECT state FROM service_signing_keys WHERE purpose = ? AND state IN ('current', 'next')").all(this.#purpose) as { state: string }[];
      for (const state of ['current', 'next'] as const) if (!have.some((k) => k.state === state)) await this.#newKey(state);
    })().catch((err: unknown) => {
      this.#ready = null;
      throw err;
    });
    await this.#ready;
  }

  async #newKey(state: 'current' | 'next'): Promise<void> {
    const { privateKey, publicKey } = await generateKeyPair('ES256', { extractable: true });
    const kid = `${this.#purpose}-${randomUUID()}`;
    const pub = { ...(await exportJWK(publicKey)), kid, alg: 'ES256', use: 'sig' };
    const priv = { ...(await exportJWK(privateKey)), kid, alg: 'ES256' };
    this.#db
      .prepare('INSERT INTO service_signing_keys (kid, purpose, state, private_jwk, public_jwk, created_at) VALUES (?, ?, ?, ?, ?, ?)')
      .run(kid, this.#purpose, state, JSON.stringify(priv), JSON.stringify(pub), nowIso());
  }

  /** The key to sign with now. */
  async current(): Promise<{ kid: string; key: CryptoKey }> {
    await this.#ensure();
    const row = this.#db.prepare("SELECT kid, private_jwk FROM service_signing_keys WHERE purpose = ? AND state = 'current'").get(this.#purpose) as { kid: string; private_jwk: string };
    return { kid: row.kid, key: (await importJWK(JSON.parse(row.private_jwk) as JWK, 'ES256')) as CryptoKey };
  }

  /** Current, next, and retired keys still within the grace period. */
  async publicKeys(): Promise<{ keys: JWK[] }> {
    await this.#ensure();
    const cutoff = new Date(Date.now() - this.#graceS() * 1000).toISOString();
    const rows = this.#db
      .prepare("SELECT public_jwk FROM service_signing_keys WHERE purpose = ? AND (state != 'retired' OR retired_at >= ?) ORDER BY created_at")
      .all(this.#purpose, cutoff) as { public_jwk: string }[];
    return { keys: rows.map((r) => JSON.parse(r.public_jwk) as JWK) };
  }

  async rotate(): Promise<{ retired: string; current: string }> {
    await this.#ensure();
    const kid = (state: string) =>
      (this.#db.prepare('SELECT kid FROM service_signing_keys WHERE purpose = ? AND state = ?').get(this.#purpose, state) as { kid: string }).kid;
    const cur = kid('current');
    const next = kid('next');
    tx(this.#db, () => {
      this.#db.prepare("UPDATE service_signing_keys SET state = 'retired', retired_at = ? WHERE kid = ?").run(nowIso(), cur);
      this.#db.prepare("UPDATE service_signing_keys SET state = 'current' WHERE kid = ?").run(next);
    });
    await this.#newKey('next');
    return { retired: cur, current: next };
  }
}
