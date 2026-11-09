import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { ApiError } from '../src/http.ts';
import { parsePublicationInput, requirePermittedPublicationSources } from '../src/knowledge/publication-input.ts';

function example() {
  return {
    expected_revision: 0, operation_id: 'operation-1', title: 'Český název', groups: ['engineering'],
    entries: [{ id: 'entry-1', text: 'Öffentliche Aussage', source_kinds: ['files', 'notes'],
      evidence: [{ id: 'evidence-1', title: 'Source', text: 'Evidence 😀' }] }],
  };
}

function rejects(value: unknown, status = 400) {
  assert.throws(() => parsePublicationInput(value, 2_000_000), (err: unknown) =>
    err instanceof ApiError && err.status === status && !err.message.includes('secret'));
}

describe('publication input', () => {
  it('retains exactly the reviewed content and supports empty evidence', () => {
    const value = example();
    assert.deepEqual(parsePublicationInput(value, 2_000_000), value);
    value.entries[0]!.evidence = [];
    assert.deepEqual(parsePublicationInput(value, 2_000_000), value);
    const result = parsePublicationInput(value, 2_000_000);
    value.entries[0]!.source_kinds.push('mail');
    assert.deepEqual(result.entries[0]!.source_kinds, ['files', 'notes']);
  });

  it('rejects private or unrecognized fields at every nesting level', () => {
    for (const target of ['publication', 'entry', 'evidence']) {
      const value = example();
      const obj = target === 'publication' ? value : target === 'entry' ? value.entries[0]! : value.entries[0]!.evidence[0]!;
      Object.assign(obj, { local_path: 'secret' });
      rejects(value);
    }
    const missing = example() as Partial<ReturnType<typeof example>>;
    delete missing.title;
    rejects(missing);
  });

  it('rejects invalid revisions, ids, Unicode and malformed lists', () => {
    for (const revision of [-1, 0.5, Number.MAX_SAFE_INTEGER + 1, NaN, '1']) {
      const value = example(); Object.assign(value, { expected_revision: revision }); rejects(value);
    }
    for (const name of ['', ' ', '\ud800', '\udc00', 'x'.repeat(301)]) {
      const value = example(); value.title = name; rejects(value);
    }
    for (const badId of ['', 'a/b', 'ok\n', 'ö', 'x'.repeat(201)]) {
      const value = example(); value.operation_id = badId; rejects(value);
    }
    for (const kinds of [[], ['files', 'files'], ['invented'], ['files', 1]]) {
      const value = example(); Object.assign(value.entries[0]!, { source_kinds: kinds }); rejects(value);
    }
    for (const groups of [[], ['group', 'group'], ['group', ''], Array(101).fill('group')]) {
      const value = example(); value.groups = groups; rejects(value);
    }
    for (const bad of [null, [], 'abc', 5]) rejects(bad);
  });

  it('rejects duplicate ids across entries and evidence', () => {
    const duplicateEntry = example();
    duplicateEntry.entries.push({ ...duplicateEntry.entries[0]!, evidence: [] });
    rejects(duplicateEntry);
    const duplicateEvidence = example();
    duplicateEvidence.entries[0]!.evidence.push({ ...duplicateEvidence.entries[0]!.evidence[0]! });
    rejects(duplicateEvidence);
    const collision = example(); collision.entries[0]!.evidence[0]!.id = collision.entries[0]!.id;
    rejects(collision);
    const laterCollision = example();
    laterCollision.entries.push({ id: 'evidence-1', text: 'Next', source_kinds: ['files'], evidence: [] });
    rejects(laterCollision);
  });

  it('requires every declared Source kind to be currently permitted', () => {
    const value = example();
    value.entries[0]!.source_kinds.push('mail');
    const parsed = parsePublicationInput(value, 2_000_000);
    assert.throws(() => requirePermittedPublicationSources(parsed, new Set(['files', 'notes'])),
      (err: unknown) => err instanceof ApiError && err.status === 403 && err.code === 'SOURCE_NOT_PERMITTED');
    assert.doesNotThrow(() => requirePermittedPublicationSources(parsed, new Set(['files', 'notes', 'mail'])));
    assert.throws(() => requirePermittedPublicationSources(parsed, new Set()), (err: unknown) => err instanceof ApiError && err.status === 403);
  });

  it('bounds entries, excerpts and the complete encoded payload before use', () => {
    const tooManyEntries = example();
    tooManyEntries.entries = Array.from({ length: 501 }, (_, i) => ({ id: `e${i}`, text: 'x', source_kinds: ['files'], evidence: [] }));
    rejects(tooManyEntries);
    const tooManyExcerpts = example();
    tooManyExcerpts.entries[0]!.evidence = Array.from({ length: 2001 }, (_, i) => ({ id: `x${i}`, title: 'T', text: 'x' }));
    rejects(tooManyExcerpts);
    const value = example();
    const bytes = Buffer.byteLength(JSON.stringify(value), 'utf8');
    assert.deepEqual(parsePublicationInput(value, bytes), value);
    assert.throws(() => parsePublicationInput(value, bytes - 1), (err: unknown) => err instanceof ApiError && err.status === 413);
    const large = example(); large.entries[0]!.text = 'x'.repeat(2_000_001);
    rejects(large, 413);
  });

  it('rejects deterministic mutations containing unknown keys or malformed values', () => {
    // A repeatable mutation corpus catches accidental coercion and shallow validation.
    const badValues: unknown[] = [null, false, 0, {}, [], '\ud800', ''];
    for (let i = 0; i < 700; i++) {
      const value = example();
      const target = i % 3 === 0 ? value : i % 3 === 1 ? value.entries[0]! : value.entries[0]!.evidence[0]!;
      Object.assign(target, { [`private_${i}`]: badValues[i % badValues.length] });
      rejects(value);
    }
  });
});
