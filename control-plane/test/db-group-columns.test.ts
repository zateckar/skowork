import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { openDb, type Db } from '../src/db.ts';

test('a database created with the earlier Group column names is renamed on start', () => {
  const dir = mkdtempSync(join(tmpdir(), 'skowork-db-'));
  const path = join(dir, 'skowork.db');
  let db: Db | undefined;
  try {
    db = openDb(path);
    db.prepare('INSERT INTO group_members (user_name, skowork_group) VALUES (?, ?)').run('alice', 'engineering');
    db.prepare('INSERT INTO role_mappings (idp_role, skowork_group) VALUES (?, ?)').run('dev', 'engineering');
    const { version } = db.prepare('SELECT version FROM schema_version').get() as { version: number };
    db.exec(`
      ALTER TABLE role_mappings RENAME COLUMN skowork_group TO workbench_group;
      ALTER TABLE group_members RENAME COLUMN skowork_group TO workbench_group;
      ALTER TABLE mcp_assignments RENAME COLUMN skowork_group TO workbench_group;
    `);
    db.prepare('UPDATE schema_version SET version = ?').run(version - 1);
    db.close();

    db = openDb(path);
    assert.deepEqual(
      db.prepare('SELECT user_name, skowork_group FROM group_members').all().map((r) => ({ ...r })),
      [{ user_name: 'alice', skowork_group: 'engineering' }],
    );
    assert.deepEqual(
      db.prepare('SELECT idp_role, skowork_group FROM role_mappings').all().map((r) => ({ ...r })),
      [{ idp_role: 'dev', skowork_group: 'engineering' }],
    );
    for (const table of ['role_mappings', 'group_members', 'mcp_assignments']) {
      const names = (db.prepare(`PRAGMA table_info(${table})`).all() as { name: string }[]).map((c) => c.name);
      assert.ok(names.includes('skowork_group'), `${table} has skowork_group`);
      assert.ok(!names.includes('workbench_group'), `${table} has no workbench_group`);
    }
  } finally {
    db?.close();
    rmSync(dir, { recursive: true, force: true, maxRetries: 5 });
  }
});
