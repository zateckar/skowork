// Remote task administration (control-plane/admin-console Remote Task Administration), as tabs of records: the
// dispatch rules of control-plane/policy-service, and the per-issuer rate limits and callback allowlist of
// control-plane/task-dispatch (redesign-admin-console-records D3).
import { useState } from 'react';
import { api } from '../auth.ts';
import { ADMIN_ROLES, ask, Dialog, Field, notify, Picker, Records, Tabs, time, useDialog, useGroups, useLoad, useTab, useUsers, type Option } from '../ui.tsx';

const R = '/api/v1/admin/policy/dispatch-rules';
const T = '/api/v1/admin/tasks';
const SP = '/api/v1/admin/identity/service-principals';
const TABS = [{ id: 'rules', label: 'Dispatch rules' }, { id: 'limits', label: 'Rate limits' }, { id: 'callbacks', label: 'Callbacks' }];

const ISSUER_KINDS: { kind: string; label: string }[] = [
  { kind: 'service-principal', label: 'A service principal' },
  { kind: 'user', label: 'A user' },
  { kind: 'group', label: 'Members of a Group' },
  { kind: 'group-owner', label: 'Owners of a Group' },
  { kind: 'role', label: 'Holders of an administrator role' },
];
const ISSUER_TEXT: Record<string, string> = { 'service-principal': 'service principal', user: 'user', group: 'members of', 'group-owner': 'owners of', role: 'role' };

function usePrincipals(): Option[] {
  const p = useLoad<{ name: string; client_id: string; retired_at: string | null }[]>(SP);
  return (p.data ?? []).filter((x) => !x.retired_at).map((x) => ({ value: x.name, hint: x.client_id }));
}

export function Remote() {
  const [tab, setTab] = useTab('remote', TABS.map((t) => t.id));
  return (
    <>
      <Tabs tabs={TABS} value={tab} onChange={setTab} />
      {tab === 'rules' && <Rules />}
      {tab === 'limits' && <Limits />}
      {tab === 'callbacks' && <Callbacks />}
    </>
  );
}

function Rules() {
  const rules = useLoad<{ id: number; issuer_kind: string; issuer: string; target_kind: string; target: string }[]>(R);
  const users = useUsers();
  const groups = useGroups();
  const principals = usePrincipals();
  const [dlg, open, close] = useDialog();
  const [issuerKind, setIssuerKind] = useState('service-principal');
  const [targetKind, setTargetKind] = useState('group');
  const issuerOptions: Record<string, Option[]> = {
    'service-principal': principals, user: users, group: groups, 'group-owner': groups, role: ADMIN_ROLES.map((r) => ({ value: r.role, hint: r.what })),
  };
  return (
    <>
      <Records
        title="Dispatch rules"
        id="dispatch-rules"
        sub="A Remote task is accepted only when a rule matches its issuer and its target user."
        items={rules.data}
        error={rules.error}
        text={(r) => `${r.issuer_kind} ${r.issuer} ${r.target_kind} ${r.target}`}
        head={<tr><th>Who may send</th><th>To whom</th><th /></tr>}
        add={<button className="primary" data-testid="rule-add-open" onClick={() => (setIssuerKind('service-principal'), setTargetKind('group'), open(true))}>Add rule</button>}
        empty="No rule: every Remote task is refused."
        row={(r) => (
          <tr key={r.id} data-testid={`rule-${r.issuer}`}>
            <td>{r.issuer_kind} <b>{r.issuer}</b></td>
            <td>{r.target_kind === 'any' ? 'all users' : <>{r.target_kind} <b>{r.target}</b></>}</td>
            <td className="acts">
              <button className="link danger" onClick={async () => {
                if ((await ask({ title: 'Remove this dispatch rule?', body: <p>Remote tasks from {ISSUER_TEXT[r.issuer_kind]} {r.issuer} to {r.target_kind === 'any' ? 'all users' : `${r.target_kind} ${r.target}`} are refused from then on, unless another rule matches.</p>, action: 'Remove rule' })) === null) return;
                await api('DELETE', `${R}/${r.id}`);
                rules.reload();
                notify('Rule removed.', 'remote-note');
              }}>Remove</button>
            </td>
          </tr>
        )}
      />
      {dlg && (
        <Dialog id="rule-add" title="Add a dispatch rule" submit="Add rule" onClose={close} noteId="remote-note"
          onSubmit={async (f) => {
            const tk = f.target_kind ?? targetKind;
            await api('POST', R, { issuer_kind: f.issuer_kind ?? issuerKind, issuer: f.issuer, target_kind: tk, target: tk === 'any' ? '' : f.target });
            rules.reload();
            return 'Rule added.';
          }}>
          <div className="two">
            <Field label="Who may send">
              <select name="issuer_kind" value={issuerKind} onChange={(e) => setIssuerKind(e.target.value)}>
                {ISSUER_KINDS.map((k) => <option key={k.kind} value={k.kind}>{k.label}</option>)}
              </select>
            </Field>
            <Field label="Issuer">
              <Picker key={issuerKind} name="issuer" options={issuerOptions[issuerKind] ?? []} allowNew={issuerKind !== 'role'} newHint="not known yet" required testid="rule-issuer" label="Issuer" />
            </Field>
          </div>
          <div className="two">
            <Field label="To whom">
              <select name="target_kind" value={targetKind} onChange={(e) => setTargetKind(e.target.value)}>
                <option value="group">Members of a Group</option><option value="user">A user</option><option value="any">All users</option>
              </select>
            </Field>
            {targetKind !== 'any' && (
              <Field label="Target">
                <Picker key={targetKind} name="target" options={targetKind === 'user' ? users : groups} allowNew newHint="not known yet" required testid="rule-target" label="Target" />
              </Field>
            )}
          </div>
        </Dialog>
      )}
    </>
  );
}

function Limits() {
  const limits = useLoad<{ default_per_hour: number; limits: { issuer: string; per_hour: number; updated_by: string; updated_at: string }[] }>(`${T}/limits`);
  const principals = usePrincipals();
  const users = useUsers();
  const [dlg, open, close] = useDialog<{ issuer?: string; per_hour?: number }>();
  const issuers: Option[] = [...principals.map((p) => ({ value: `service:${p.value}`, hint: 'service principal' })), ...users.map((u) => ({ value: `user:${u.value}`, hint: 'user' }))];
  const def = limits.data?.default_per_hour ?? 100;
  return (
    <>
      <Records
        title="Issuer rate limits"
        id="issuer-limits"
        sub={`Remote tasks each issuer may send per hour. Issuers without their own limit get ${def}.`}
        items={limits.data?.limits}
        error={limits.error}
        text={(l) => l.issuer}
        head={<tr><th>Issuer</th><th>Tasks per hour</th><th>Changed</th><th /></tr>}
        add={<button className="primary" data-testid="limit-set-open" onClick={() => open({})}>Set a limit</button>}
        empty={`Every issuer gets the default of ${def} tasks per hour.`}
        row={(l) => (
          <tr key={l.issuer} data-testid={`limit-${l.issuer}`}>
            <td><b>{l.issuer}</b></td><td>{l.per_hour}</td><td>{l.updated_by}<small>{time(l.updated_at)}</small></td>
            <td className="acts">
              <button className="link" onClick={() => open(l)}>Change</button>
              <button className="link danger" onClick={async () => {
                if ((await ask({ title: `Remove the limit of ${l.issuer}?`, body: <p>It gets the default of {def} tasks per hour.</p>, action: 'Remove limit' })) === null) return;
                await api('DELETE', `${T}/limits/${encodeURIComponent(l.issuer)}`);
                limits.reload();
                notify('Limit removed.', 'remote-note');
              }}>Remove</button>
            </td>
          </tr>
        )}
      />
      {dlg && (
        <Dialog id="limit-set" title={dlg.issuer ? `Limit of ${dlg.issuer}` : 'Set a rate limit'} submit="Save limit" onClose={close} noteId="remote-note"
          onSubmit={async (f) => {
            await api('PUT', `${T}/limits`, { issuer: dlg.issuer ?? f.issuer, per_hour: Number(f.per_hour) });
            limits.reload();
            return 'Limit saved. It applies to the next request.';
          }}>
          {!dlg.issuer && (
            <Field label="Issuer" help="service:<name> for a service principal, user:<user name> for a person.">
              <Picker name="issuer" options={issuers} allowNew newHint="not known yet" required testid="limit-issuer" label="Issuer" />
            </Field>
          )}
          <Field label="Tasks per hour"><input name="per_hour" type="number" min="0" defaultValue={dlg.per_hour ?? def} required /></Field>
        </Dialog>
      )}
    </>
  );
}

function Callbacks() {
  const callbacks = useLoad<{ prefix: string; added_by: string; added_at: string }[]>(`${T}/callbacks`);
  const [dlg, open, close] = useDialog();
  return (
    <>
      <Records
        title="Callback allowlist"
        id="callback-allowlist"
        sub="A Remote task may name a result callback only under one of these URL prefixes."
        items={callbacks.data}
        error={callbacks.error}
        text={(c) => c.prefix}
        head={<tr><th>URL prefix</th><th>Added</th><th /></tr>}
        add={<button className="primary" data-testid="callback-add-open" onClick={() => open(true)}>Add prefix</button>}
        empty="No prefix is allowed, so no task may name a callback."
        row={(c) => (
          <tr key={c.prefix} data-testid={`callback-${c.prefix}`}>
            <td><code>{c.prefix}</code></td><td>{c.added_by}<small>{time(c.added_at)}</small></td>
            <td className="acts">
              <button className="link danger" onClick={async () => {
                if ((await ask({ title: 'Remove this prefix?', body: <p>Tasks can no longer call back to <code>{c.prefix}</code>.</p>, action: 'Remove prefix' })) === null) return;
                await api('POST', `${T}/callbacks/remove`, { prefix: c.prefix });
                callbacks.reload();
                notify('Prefix removed.', 'remote-note');
              }}>Remove</button>
            </td>
          </tr>
        )}
      />
      {dlg && (
        <Dialog id="callback-add" title="Allow a callback prefix" submit="Add prefix" onClose={close} noteId="remote-note"
          onSubmit={async (f) => {
            await api('POST', `${T}/callbacks`, { prefix: f.prefix });
            callbacks.reload();
            return 'Prefix added.';
          }}>
          <Field label="URL prefix" help="HTTPS, ending with a slash, so that it matches one path and what is below it.">
            <input name="prefix" type="url" placeholder="https://ci.example.com/hooks/" required />
          </Field>
        </Dialog>
      )}
    </>
  );
}
