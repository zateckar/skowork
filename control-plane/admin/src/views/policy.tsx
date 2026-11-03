// Policy administration (control-plane/admin-console Policy Administration) over the policy-service API: Groups,
// drafts with the service's validation errors next to the fields, the Policy Diff, publishing with staged rollout,
// adoption by version, and simulation with the source of each setting.
import { useEffect, useState } from 'react';
import { api, ApiError } from '../auth.ts';
import type { Me } from '../App.tsx';
import { Card, confirmed, Err, Field, Form, Note, show, time, useAction, useLoad } from '../ui.tsx';
import { PolicyForm } from './policy-form.tsx';

interface Group { name: string; defined: boolean; description: string; owners: string[]; current_version: number | null; has_draft?: boolean }
interface Versions {
  current_version: number | null;
  previous_version: number | null;
  candidate_version: number | null;
  rollout: { kind: string; value: string | number } | null;
  versions: { version: number; priority: number; created_by: string; created_at: string; reason: string; draft: boolean }[];
}
interface Adoption {
  installations: number;
  current_version: number | null;
  candidate_version: number | null;
  rollout: { kind: string; value: string | number; selected_users: number } | null;
  by_version: { version: number; installations: number }[];
  not_updated: number;
}
export interface FieldError { path?: string; keys?: string[]; message: string }

const P = '/api/v1/admin/policy';
export const scopePath = (scope: string) => (scope === 'guardrail' ? `${P}/guardrail` : `${P}/groups/${encodeURIComponent(scope)}`);

/** Leaf paths of a document ("tools.shell.mode" → value); arrays are leaves. */
export function flatten(doc: unknown, prefix = ''): [string, unknown][] {
  if (doc === null || typeof doc !== 'object' || Array.isArray(doc)) return prefix ? [[prefix, doc]] : [];
  const out: [string, unknown][] = [];
  for (const [k, v] of Object.entries(doc as Record<string, unknown>)) out.push(...flatten(v, prefix ? `${prefix}.${k}` : k));
  if (!out.length && prefix) out.push([prefix, doc]);
  return out;
}

/** The document field by field, with each validation error next to the field it names. */
export function FieldErrors({ doc, errors }: { doc: unknown; errors: FieldError[] }) {
  const pathOf = (e: FieldError) => e.path ?? e.keys?.[0] ?? '';
  const used = new Set<FieldError>();
  const rows = flatten(doc).map(([path, value]) => {
    const errs = errors.filter((e) => {
      const p = pathOf(e);
      return p !== '' && (p === path || path.startsWith(`${p}.`) || p.startsWith(`${path}.`));
    });
    errs.forEach((e) => used.add(e));
    return { path, value, errs };
  });
  const rest = errors.filter((e) => !used.has(e));
  return (
    <table className="fields" data-testid="field-errors">
      <thead><tr><th>Field</th><th>Value</th><th>Problem</th></tr></thead>
      <tbody>
        {rows.map((r) => (
          <tr key={r.path} className={r.errs.length ? 'bad' : ''}>
            <td>policy.{r.path}</td><td><code>{show(r.value)}</code></td>
            <td className="err" data-testid={r.errs.length ? `field-error-${r.path}` : undefined}>{r.errs.map((e) => e.message).join('; ')}</td>
          </tr>
        ))}
        {rest.map((e, i) => (
          <tr key={`rest-${i}`} className="bad"><td>{pathOf(e) ? `policy.${pathOf(e)}` : '(document)'}</td><td /><td className="err" data-testid={`field-error-${pathOf(e) || 'document'}`}>{e.message}</td></tr>
        ))}
      </tbody>
    </table>
  );
}

export function Policy({ me }: { me: Me }) {
  const groups = useLoad<Group[]>(`${P}/groups`);
  const [scope, setScope] = useState('guardrail');
  const platform = me.admin_roles.includes('platform-admin');
  const canEdit = platform || me.admin_roles.includes('group-owner');
  const define = useAction();
  return (
    <div className="split">
      <aside className="card">
        <h2>Scopes</h2>
        <p className="muted">The org guardrail applies to everyone and wins over Groups. Pick a scope to edit its policy.</p>
        <ul className="pick">
          <li><button className={scope === 'guardrail' ? 'active' : ''} data-testid="scope-guardrail" onClick={() => setScope('guardrail')}>Org guardrail</button></li>
          {groups.data?.map((g) => (
            <li key={g.name}>
              <button className={scope === g.name ? 'active' : ''} data-testid={`scope-${g.name}`} onClick={() => setScope(g.name)}>
                {g.name}{g.owners.length ? <small> · {g.owners.join(', ')}</small> : null}
                {!g.current_version && !g.has_draft ? <small> · no policy</small> : !g.defined ? <small> · not defined</small> : null}
              </button>
            </li>
          ))}
        </ul>
        <Err error={groups.error} />
        {platform && (
          <Form id="define-group" onSubmit={(f) => define.run(async () => {
            await api('PUT', `${P}/groups/${encodeURIComponent(f.name ?? '')}`, { description: f.description, owners: (f.owners ?? '').split(',').map((s) => s.trim()).filter(Boolean) });
            groups.reload();
            setScope(f.name ?? 'guardrail');
          })}>
            <h3>New Group</h3>
            <Field label="Name"><input name="name" required /></Field>
            <Field label="Description"><input name="description" /></Field>
            <Field label="Owners (comma-separated)"><input name="owners" /></Field>
            <button>Define Group</button>
          </Form>
        )}
        <Err error={define.error} />
      </aside>
      <div className="stack">
        <ScopePanel key={scope} scope={scope} canEdit={canEdit} onChanged={groups.reload} />
        <Simulation />
      </div>
    </div>
  );
}

function ScopePanel({ scope, canEdit, onChanged }: { scope: string; canEdit: boolean; onChanged: () => void }) {
  const base = scopePath(scope);
  const versions = useLoad<Versions>(`${base}/versions`);
  const adoption = useLoad<Adoption>(`${base}/adoption`);
  const [text, setText] = useState('');
  const [priority, setPriority] = useState(0);
  const [errors, setErrors] = useState<{ doc: unknown; errors: FieldError[] } | null>(null);
  const [diff, setDiff] = useState<any>(null);
  const [mode, setMode] = useState<'form' | 'json'>('form');
  const [syntax, setSyntax] = useState('');
  const act = useAction();
  let parsed: Record<string, unknown> | null = null;
  try {
    const p = JSON.parse(text || '{}');
    if (p && typeof p === 'object' && !Array.isArray(p)) parsed = p;
  } catch { /* shown when switching to the form */ }
  const toForm = () => (parsed ? (setSyntax(''), setMode('form')) : (setSyntax('The JSON has a syntax error; fix it before switching to the form.'), setMode('json')));

  // The editor starts from the draft, else from the version in force, else empty.
  useEffect(() => {
    (async () => {
      for (const ref of ['draft', 'current']) {
        try {
          const v = await api<{ document: unknown; priority: number }>('GET', `${base}/versions/${ref}`);
          setText(JSON.stringify(v.document, null, 2));
          setPriority(v.priority);
          return;
        } catch { /* try the next */ }
      }
      setText('{}');
    })();
  }, [base]);

  const reload = () => (versions.reload(), adoption.reload(), onChanged());
  const save = (reason: string) => act.run(async () => {
    let doc: unknown;
    try {
      doc = JSON.parse(text);
    } catch (e) {
      throw new Error(`The document is not valid JSON: ${(e as Error).message}`);
    }
    try {
      await api('PUT', `${base}/draft`, { document: doc, priority, reason });
      setErrors(null);
    } catch (e) {
      if (e instanceof ApiError && Array.isArray(e.details.errors)) setErrors({ doc, errors: e.details.errors as FieldError[] });
      throw e;
    }
    reload();
  }, 'Draft saved. Nothing is published until you publish it.');
  const post = (path: string, body: unknown, note: string) => act.run(async () => {
    await api('POST', `${base}${path}`, body);
    reload();
  }, note);

  const v = versions.data;
  return (
    <>
      <Card title={scope === 'guardrail' ? 'Org guardrail' : `Group ${scope}`} id="scope-panel">
        <Err error={versions.error} />
        {v && (
          <p data-testid="scope-state">
            In force: <b>{v.current_version ?? 'none'}</b>
            {v.candidate_version && <> · rolling out <b>{v.candidate_version}</b> to {v.rollout?.kind === 'percent' ? `${v.rollout.value}% of users` : `pilot Group ${v.rollout?.value}`}</>}
          </p>
        )}
        <table>
          <thead><tr><th>Version</th><th>Priority</th><th>By</th><th>At</th><th>Reason</th></tr></thead>
          <tbody>
            {v?.versions.map((x) => (
              <tr key={x.version}><td>{x.draft ? 'draft' : x.version}</td><td>{x.priority}</td><td>{x.created_by}</td><td>{time(x.created_at)}</td><td>{x.reason}</td></tr>
            ))}
          </tbody>
        </table>
      </Card>

      <Card title="Adoption" id="policy-adoption" actions={<button onClick={adoption.reload}>Refresh</button>}>
        <Err error={adoption.error} />
        {adoption.data && adoption.data.current_version == null && adoption.data.candidate_version == null && (
          <p className="muted" data-testid="adoption-none">Nothing published for this scope yet, so there is nothing to adopt.</p>
        )}
        {adoption.data && (adoption.data.current_version != null || adoption.data.candidate_version != null) && (
          <ul>
            {adoption.data.by_version.map((b) => (
              <li key={b.version} data-testid={`adoption-v${b.version}`}>
                Version {b.version}{b.version === adoption.data!.candidate_version ? ' (rollout)' : b.version === adoption.data!.current_version ? ' (in force)' : ''}: {b.installations} installation(s)
              </li>
            ))}
            <li data-testid="adoption-not-updated">Not yet updated: {adoption.data.not_updated} of {adoption.data.installations}</li>
            {adoption.data.rollout && <li data-testid="adoption-selected">Users selected by the rollout: {adoption.data.rollout.selected_users}</li>}
          </ul>
        )}
      </Card>

      {canEdit && (
        <Card title="Draft" id="policy-draft" actions={
          <span className="tabs" role="tablist">
            <button type="button" role="tab" aria-selected={mode === 'form'} className={mode === 'form' ? 'active' : ''} data-testid="draft-mode-form" onClick={toForm}>Form</button>
            <button type="button" role="tab" aria-selected={mode === 'json'} className={mode === 'json' ? 'active' : ''} data-testid="draft-mode-json" onClick={() => setMode('json')}>JSON</button>
          </span>
        }>
          {mode === 'form' && parsed
            ? <PolicyForm doc={parsed} guardrail={scope === 'guardrail'} errors={errors?.errors ?? []} onChange={(d) => setText(JSON.stringify(d, null, 2))} />
            : <textarea data-testid="draft-text" aria-label="Policy document (JSON)" rows={14} spellCheck={false} value={text} onChange={(e) => setText(e.target.value)} />}
          <Err error={syntax} />
          <Form id="draft-save" onSubmit={(f) => save(f.reason ?? '')}>
            {scope !== 'guardrail' && <Field label="Priority (higher wins)"><input type="number" min={0} max={1000} value={priority} onChange={(e) => setPriority(Number(e.target.value))} /></Field>}
            <Field label="Reason" grow><input name="reason" /></Field>
            <button className="primary">Save draft</button>
            <button type="button" data-testid="diff" onClick={() => void act.run(async () => setDiff(await api('GET', `${base}/diff?from=${v?.current_version ? 'current' : 'none'}&to=draft`)))}>Diff with version in force</button>
          </Form>
          {errors && mode === 'json' && <FieldErrors doc={errors.doc} errors={errors.errors} />}
          {diff && <Diff diff={diff} />}
        </Card>
      )}

      {canEdit && (
        <Card title="Publish" id="policy-publish">
          <Form id="publish" onSubmit={(f) => post('/publish', {
            reason: f.reason,
            rollout: f.rollout === 'pilot' ? { pilot_group: f.value } : f.rollout === 'percent' ? { percent: Number(f.value) } : undefined,
          }, 'Published.')}>
            <Field label="Reason" grow><input name="reason" required /></Field>
            {scope !== 'guardrail' && (
              <>
                <Field label="Rollout"><select name="rollout" data-testid="rollout-kind"><option value="all">to everyone</option><option value="pilot">to a pilot Group</option><option value="percent">to a percentage</option></select></Field>
                <Field label="Pilot Group or %"><input name="value" /></Field>
              </>
            )}
            <button>Publish draft</button>
          </Form>
          <div className="row">
            {v?.candidate_version && (
              <>
                <button data-testid="promote" onClick={() => void post('/rollout/promote', { reason: 'promoted in admin console' }, 'Promoted to everyone.')}>Promote to everyone</button>
                <button data-testid="cancel-rollout" onClick={() => confirmed('Cancel the rollout? Users who got the new version go back to the version in force.') && void post('/rollout/cancel', { reason: 'cancelled in admin console' }, 'Rollout cancelled.')}>Cancel rollout</button>
              </>
            )}
            {!v?.candidate_version && v?.previous_version && (
              <button data-testid="rollback" onClick={() => confirmed(`Roll back to version ${v.previous_version}?`) && void post('/rollback', { reason: 'rolled back in admin console' }, 'Rolled back.')}>Roll back to {v.previous_version}</button>
            )}
          </div>
        </Card>
      )}
      <Err error={act.error} id="policy-error" />
      <Note text={act.note} id="policy-note" />
    </>
  );
}

function Diff({ diff }: { diff: { from: number; to: number; changes: { added: any[]; removed: any[]; changed: any[] } } }) {
  const c = diff.changes;
  return (
    <table data-testid="policy-diff">
      <thead><tr><th colSpan={3}>Changes from {diff.from ? `version ${diff.from}` : 'nothing in force'} to the draft</th></tr></thead>
      <tbody>
        {c.added.map((x) => <tr key={`a${x.path}`} className="add"><td>+ policy.{x.path}</td><td /><td><code>{show(x.value)}</code></td></tr>)}
        {c.removed.map((x) => <tr key={`r${x.path}`} className="del"><td>− policy.{x.path}</td><td><code>{show(x.value)}</code></td><td /></tr>)}
        {c.changed.map((x) => <tr key={`c${x.path}`}><td>~ policy.{x.path}</td><td><code>{show(x.old)}</code></td><td><code>{show(x.new)}</code></td></tr>)}
        {!c.added.length && !c.removed.length && !c.changed.length && <tr><td colSpan={3}>No changes.</td></tr>}
      </tbody>
    </table>
  );
}

function Simulation() {
  const [result, setResult] = useState<any>(null);
  const act = useAction();
  const sourceOf = (src: Record<string, string>, path: string) => {
    for (let p = path; p; p = p.includes('.') ? p.slice(0, p.lastIndexOf('.')) : '') if (src[p]) return src[p];
    return 'default';
  };
  return (
    <Card title="Simulate the effective policy" id="policy-simulate">
      <Form id="simulate" onSubmit={(f) => act.run(async () => setResult(await api('POST', `${P}/simulate`, {
        user: f.user || undefined,
        groups: f.user ? undefined : (f.groups ?? '').split(',').map((s) => s.trim()).filter(Boolean),
        drafts: f.drafts === 'on',
      })))}>
        <Field label="User"><input name="user" /></Field>
        <span className="muted" style={{ paddingBottom: '0.4rem' }}>or</span>
        <Field label="Groups (comma-separated)" grow><input name="groups" /></Field>
        <label className="check"><input type="checkbox" name="drafts" /> with drafts</label>
        <button>Simulate</button>
      </Form>
      <Err error={act.error} />
      {result && (
        <>
          <p>Groups: {result.groups.join(', ')} · versions {Object.entries(result.versions).map(([k, x]) => `${k} ${x}`).join(', ') || 'none'}</p>
          {result.errors.length > 0 && <Err error={result.errors.map((e: FieldError) => e.message).join('; ')} />}
          {result.conflicts.length > 0 && (
            <ul className="err">{result.conflicts.map((c: any, i: number) => <li key={i}>policy.{c.path}: {c.group} allows {show(c.group_value)}, the guardrail decides {c.effective}</li>)}</ul>
          )}
          <table data-testid="simulation">
            <thead><tr><th>Setting</th><th>Value</th><th>Source</th></tr></thead>
            <tbody>
              {flatten(result.policy).map(([path, value]) => (
                <tr key={path} data-testid={`sim-${path}`}><td>policy.{path}</td><td><code>{show(value)}</code></td><td>{sourceOf(result.src, path)}</td></tr>
              ))}
            </tbody>
          </table>
        </>
      )}
    </Card>
  );
}
