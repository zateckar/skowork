// Policy administration (control-plane/admin-console Policy Administration) over the policy-service API: the scopes
// (org guardrail and Groups) as a list; for the chosen scope its state with the actions that change it (publish,
// promote, cancel, roll back), its versions and adoption, the draft editor with the service's validation errors next
// to the fields, and simulation. Groups are defined and edited, and drafts published, in dialogs
// (redesign-admin-console-records D3).
import { useEffect, useState } from 'react';
import { api, ApiError } from '../auth.ts';
import type { Me } from '../App.tsx';
import { ask, Card, Chips, Dialog, Err, Field, Form, list, Note, notify, Picker, refreshKnown, show, time, useAction, useDialog, useGroups, useLoad, useUsers } from '../ui.tsx';
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
  const users = useUsers();
  const [scope, setScope] = useState('guardrail');
  const [groupDlg, openGroup, closeGroup] = useDialog<Group | 'new'>();
  const platform = me.admin_roles.includes('platform-admin');
  const canEdit = platform || me.admin_roles.includes('group-owner');
  const group = groups.data?.find((g) => g.name === scope);
  return (
    <div className="split">
      <aside className="card">
        <div className="card-head">
          <h2>Scopes</h2>
          {platform && <button className="primary" data-testid="define-group-open" onClick={() => openGroup('new')}>New Group</button>}
        </div>
        <p className="muted sub">The org guardrail applies to everyone and wins over Groups.</p>
        <ul className="scopes">
          <li><button className={scope === 'guardrail' ? 'active' : ''} data-testid="scope-guardrail" onClick={() => setScope('guardrail')}>Org guardrail</button></li>
          {groups.data?.map((g) => (
            <li key={g.name}>
              <button className={scope === g.name ? 'active' : ''} data-testid={`scope-${g.name}`} onClick={() => setScope(g.name)}>
                <span>{g.name}</span>
                <small className="muted">{!g.current_version && !g.has_draft ? 'no policy' : !g.defined ? 'not defined' : g.current_version ? `v${g.current_version}` : 'draft'}</small>
              </button>
            </li>
          ))}
        </ul>
        <Err error={groups.error} />
      </aside>
      <div className="stack">
        <ScopePanel
          key={scope}
          scope={scope}
          group={group}
          canEdit={canEdit}
          onEditGroup={platform && group ? () => openGroup(group) : undefined}
          onChanged={groups.reload}
        />
        <Simulation />
      </div>

      {groupDlg && (
        <Dialog
          id={groupDlg === 'new' ? 'define-group' : 'edit-group'}
          title={groupDlg === 'new' ? 'New Group' : `Edit Group ${groupDlg.name}`}
          submit={groupDlg === 'new' ? 'Define Group' : 'Save'}
          onClose={closeGroup}
          noteId="policy-note"
          intro={groupDlg === 'new' ? <p>A Group gets its own policy. Its members come from the identity provider or from Group members under Identity.</p> : undefined}
          onSubmit={async (f) => {
            const name = groupDlg === 'new' ? (f.name ?? '') : groupDlg.name;
            await api('PUT', `${P}/groups/${encodeURIComponent(name)}`, { description: f.description, owners: list(f.owners) });
            refreshKnown();
            groups.reload();
            setScope(name);
            return groupDlg === 'new' ? `Group ${name} defined. It has no policy until you publish one.` : `Group ${name} saved.`;
          }}
        >
          {groupDlg === 'new' && <Field label="Name" help="As the identity provider or the Group members name it."><input name="name" required autoComplete="off" /></Field>}
          <Field label="Description"><input name="description" defaultValue={groupDlg === 'new' ? '' : groupDlg.description} /></Field>
          <Field label="Owners" help="Group owners edit and publish the policy of this Group.">
            <Picker name="owners" options={users} multiple allowNew newHint="not seen yet" defaultValue={groupDlg === 'new' ? [] : groupDlg.owners} testid="group-owners" label="Owners" />
          </Field>
        </Dialog>
      )}
    </div>
  );
}

function ScopePanel({ scope, group, canEdit, onEditGroup, onChanged }: { scope: string; group?: Group; canEdit: boolean; onEditGroup?: () => void; onChanged: () => void }) {
  const base = scopePath(scope);
  const versions = useLoad<Versions>(`${base}/versions`);
  const adoption = useLoad<Adoption>(`${base}/adoption`);
  const groups = useGroups();
  const [text, setText] = useState('');
  const [priority, setPriority] = useState(0);
  const [errors, setErrors] = useState<{ doc: unknown; errors: FieldError[] } | null>(null);
  const [diff, setDiff] = useState<any>(null);
  const [mode, setMode] = useState<'form' | 'json'>('form');
  const [syntax, setSyntax] = useState('');
  const [publishing, openPublish, closePublish] = useDialog();
  const [rollout, setRollout] = useState('all');
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
  const post = async (path: string, body: unknown, note: string) => {
    await api('POST', `${base}${path}`, body);
    reload();
    notify(note, 'policy-note');
  };

  const v = versions.data;
  const hasDraft = !!v?.versions.some((x) => x.draft);
  const title = scope === 'guardrail' ? 'Org guardrail' : `Group ${scope}`;
  return (
    <>
      <Card
        title={title}
        id="scope-panel"
        sub={scope === 'guardrail' ? 'Applies to everyone; Groups cannot loosen it.' : group?.description || undefined}
        actions={
          <>
            {onEditGroup && <button data-testid="edit-group-open" onClick={onEditGroup}>Edit Group</button>}
            {canEdit && v?.candidate_version && (
              <>
                <button data-testid="cancel-rollout" onClick={async () => {
                  const r = await ask({ title: `Cancel the rollout of version ${v.candidate_version}?`, body: <p>Users who got version {v.candidate_version} go back to the version in force{v.current_version ? ` (${v.current_version})` : ''}.</p>, action: 'Cancel rollout', reason: 'optional' });
                  if (r !== null) await post('/rollout/cancel', { reason: r || 'cancelled in admin console' }, 'Rollout cancelled.');
                }}>Cancel rollout</button>
                <button className="primary" data-testid="promote" onClick={async () => {
                  const r = await ask({ title: `Promote version ${v.candidate_version} to everyone?`, body: <p>Every user of {title} gets version {v.candidate_version} at their next policy update.</p>, action: 'Promote', danger: false, reason: 'optional' });
                  if (r !== null) await post('/rollout/promote', { reason: r || 'promoted in admin console' }, 'Promoted to everyone.');
                }}>Promote to everyone</button>
              </>
            )}
            {canEdit && !v?.candidate_version && v?.previous_version && (
              <button data-testid="rollback" onClick={async () => {
                const r = await ask({ title: `Roll back to version ${v.previous_version}?`, body: <p>Version {v.previous_version} is put in force again for everyone in {title}.</p>, action: 'Roll back', reason: 'optional' });
                if (r !== null) await post('/rollback', { reason: r || 'rolled back in admin console' }, 'Rolled back.');
              }}>Roll back to {v.previous_version}</button>
            )}
            {canEdit && !v?.candidate_version && (
              <button className="primary" data-testid="publish-open" disabled={!hasDraft} title={hasDraft ? undefined : 'Save a draft first'} onClick={() => (setRollout('all'), openPublish(true))}>Publish draft</button>
            )}
          </>
        }
      >
        <Err error={versions.error} />
        {v && (
          <div className="facts">
            <div data-testid="scope-state">
              <span>Version</span>
              <b>In force: {v.current_version ?? 'none'}{v.candidate_version && <> · rolling out {v.candidate_version} to {v.rollout?.kind === 'percent' ? `${v.rollout.value}% of users` : `pilot Group ${v.rollout?.value}`}</>}</b>
            </div>
            <div><span>Draft</span><b>{hasDraft ? 'unpublished changes' : 'none'}</b></div>
            {group && <div><span>Owners</span><Chips values={group.owners} empty="none" /></div>}
          </div>
        )}
        {v && v.versions.length > 0 && (
          <div className="scroll">
            <table>
              <thead><tr><th>Version</th><th>Priority</th><th>By</th><th>When</th><th>Reason</th></tr></thead>
              <tbody>
                {v.versions.map((x) => (
                  <tr key={x.version}>
                    <td>{x.draft ? <span className="state pending">draft</span> : <b>{x.version}</b>}{!x.draft && x.version === v.current_version && <small>in force</small>}{!x.draft && x.version === v.candidate_version && <small>rolling out</small>}</td>
                    <td>{x.priority}</td><td>{x.created_by}</td><td>{time(x.created_at)}</td><td>{x.reason}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Card>

      <Card title="Adoption" id="policy-adoption" sub="Installations by the version they run." actions={<button onClick={adoption.reload}>Refresh</button>}>
        <Err error={adoption.error} />
        {adoption.data && adoption.data.current_version == null && adoption.data.candidate_version == null && (
          <p className="muted" data-testid="adoption-none">Nothing published for this scope yet, so there is nothing to adopt.</p>
        )}
        {adoption.data && (adoption.data.current_version != null || adoption.data.candidate_version != null) && (
          <ul className="entries">
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
        <Card title="Draft" id="policy-draft" sub="Edit the next version. Saving does not publish it." actions={
          <span className="tabs" role="tablist">
            <button type="button" role="tab" aria-selected={mode === 'form'} className={mode === 'form' ? 'active' : ''} data-testid="draft-mode-form" onClick={toForm}>Form</button>
            <button type="button" role="tab" aria-selected={mode === 'json'} className={mode === 'json' ? 'active' : ''} data-testid="draft-mode-json" onClick={() => setMode('json')}>JSON</button>
          </span>
        }>
          {mode === 'form' && parsed
            ? <PolicyForm doc={parsed} guardrail={scope === 'guardrail'} errors={errors?.errors ?? []} onChange={(d) => setText(JSON.stringify(d, null, 2))} />
            : <textarea data-testid="draft-text" aria-label="Policy document (JSON)" rows={14} spellCheck={false} value={text} onChange={(e) => setText(e.target.value)} />}
          <Err error={syntax} />
          {errors && mode === 'json' && <FieldErrors doc={errors.doc} errors={errors.errors} />}
          <Form id="draft-save" onSubmit={(f) => save(f.reason ?? '')}>
            {scope !== 'guardrail' && <Field label="Priority (higher wins)"><input type="number" min={0} max={1000} value={priority} onChange={(e) => setPriority(Number(e.target.value))} /></Field>}
            <Field label="Reason for the change" grow><input name="reason" /></Field>
            <button type="button" data-testid="diff" onClick={() => void act.run(async () => setDiff(await api('GET', `${base}/diff?from=${v?.current_version ? 'current' : 'none'}&to=draft`)))}>Compare with version in force</button>
            <button className="primary">Save draft</button>
          </Form>
        </Card>
      )}
      <Err error={act.error} id="policy-error" />
      <Note text={act.note} id="policy-note" />

      {diff && (
        <Dialog id="diff-dialog" title={`Changes in the draft of ${title}`} wide onClose={() => setDiff(null)}>
          <Diff diff={diff} />
        </Dialog>
      )}

      {publishing && (
        <Dialog id="publish" title={`Publish the draft of ${title}`} submit="Publish" onClose={closePublish} noteId="policy-note"
          intro={<p>The draft becomes the next version. Clients get it at their next policy update.</p>}
          onSubmit={async (f) => {
            const kind = f.rollout ?? 'all';
            await api('POST', `${base}/publish`, {
              reason: f.reason,
              rollout: kind === 'pilot' ? { pilot_group: f.value } : kind === 'percent' ? { percent: Number(f.value) } : undefined,
            });
            reload();
            return kind === 'all' ? 'Published to everyone.' : 'Published. The rollout has started.';
          }}>
          <Field label="Reason" help="Kept with the version in the audit log."><input name="reason" required /></Field>
          {scope !== 'guardrail' && (
            <div className="two">
              <Field label="Roll out">
                <select name="rollout" data-testid="rollout-kind" value={rollout} onChange={(e) => setRollout(e.target.value)}>
                  <option value="all">To everyone now</option><option value="pilot">To a pilot Group first</option><option value="percent">To a share of users first</option>
                </select>
              </Field>
              {rollout === 'pilot' && (
                <Field label="Pilot Group">
                  <Picker name="value" options={groups} allowNew newHint="not known yet" required testid="rollout-pilot" label="Pilot Group" />
                </Field>
              )}
              {rollout === 'percent' && <Field label="Share of users (%)"><input name="value" type="number" min={1} max={99} defaultValue={10} required /></Field>}
            </div>
          )}
        </Dialog>
      )}
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
  const users = useUsers();
  const groups = useGroups();
  const catalog = useLoad<{ built_in_tools?: string[]; discovered_tools?: string[]; tool_details?: Record<string, { description?: string }> }>(`${P}/schema?keys=true`);
  const tools = [...(catalog.data?.built_in_tools ?? []), ...(catalog.data?.discovered_tools ?? [])]
    .map((value) => ({ value, hint: catalog.data?.tool_details?.[value]?.description }));
  const act = useAction();
  const sourceOf = (src: Record<string, string>, path: string) => {
    for (let p = path; p; p = p.includes('.') ? p.slice(0, p.lastIndexOf('.')) : '') if (src[p]) return src[p];
    return 'default';
  };
  return (
    <Card title="Simulate the effective policy" id="policy-simulate" sub="See what a user, or a member of some Groups, gets, and where each setting comes from.">
      <Form id="simulate" onSubmit={(f) => act.run(async () => setResult(await api('POST', `${P}/simulate`, {
        user: f.user || undefined,
        groups: f.user ? undefined : list(f.groups),
        drafts: f.drafts === 'on',
        tool_call: f.tool ? { tool: f.tool, risk: f.risk, unattended: f.unattended === 'on' } : undefined,
      })))}>
        <Field label="User"><Picker name="user" options={users} allowNew newHint="not seen yet" testid="sim-user" label="User" placeholder="Pick a user" /></Field>
        <span className="muted" style={{ paddingBottom: '0.5rem' }}>or</span>
        <Field label="Groups" grow><Picker name="groups" options={groups} multiple allowNew newHint="not known yet" testid="sim-groups" label="Groups" /></Field>
        <Field label="Tool (optional)" help="Choose a tool to see a policy decision for an example call."><Picker name="tool" options={tools} allowNew newHint="custom tool" testid="sim-tool" label="Tool" placeholder="Search tools" /></Field>
        <Field label="Assumed action risk"><select name="risk" defaultValue="read" data-testid="sim-risk"><option value="read">Read</option><option value="write-local">Write locally</option><option value="external">External</option><option value="destructive">Destructive</option></select></Field>
        <label className="check"><input type="checkbox" name="unattended" data-testid="sim-unattended" /> unattended run</label>
        <label className="check"><input type="checkbox" name="drafts" /> include saved drafts</label>
        <button className="primary">Simulate</button>
      </Form>
      <Err error={act.error} />
      {result && (
        <>
          <p>Groups: {result.groups.join(', ') || 'none'} · versions {Object.entries(result.versions).map(([k, x]) => `${k} ${x}`).join(', ') || 'none'}</p>
          {result.errors.length > 0 && <Err error={result.errors.map((e: FieldError) => e.message).join('; ')} />}
          {result.conflicts.length > 0 && (
            <ul className="err">{result.conflicts.map((c: any, i: number) => <li key={i}>policy.{c.path}: {c.group} allows {show(c.group_value)}, the guardrail decides {c.effective}</li>)}</ul>
          )}
          {result.tool_decision_error && <Err error={result.tool_decision_error} />}
          {result.tool_decision && <section data-testid="simulation-tool-decision">
            <h3>Example tool decision: {result.tool_decision.decision}</h3>
            <p><code>{result.tool_decision.tool}</code> · assumed risk: {result.tool_decision.risk} · {result.tool_decision.unattended ? 'unattended' : 'attended'} · {result.drafts ? 'including saved drafts' : 'published policy'}</p>
            <p>Decision from <code>{result.tool_decision.rule}</code> ({result.tool_decision.source || 'policy'}): {result.tool_decision.reason}</p>
            <ol>{result.tool_decision.steps.map((step: any, i: number) => <li key={i}>{step.stage}: {step.decision} — {step.reason} <small>({step.source || 'policy'}: {step.rule})</small>
              {step.considered && <div>Considered: {step.considered.decision} from <code>{step.considered.rule}</code> ({step.considered.source || 'policy'}) — {step.considered.reason}</div>}</li>)}</ol>
            <ul>{result.tool_decision.conditions.map((condition: string) => <li key={condition}>{condition}</li>)}</ul>
          </section>}
          <div className="scroll">
            <table data-testid="simulation">
              <thead><tr><th>Setting</th><th>Value</th><th>Source</th></tr></thead>
              <tbody>
                {flatten(result.policy).map(([path, value]) => (
                  <tr key={path} data-testid={`sim-${path}`}><td>policy.{path}</td><td><code>{show(value)}</code></td><td>{sourceOf(result.src, path)}</td></tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
    </Card>
  );
}
