// Identity administration (control-plane/admin-console Identity Administration) over control-plane/identity, as
// tabs of records with dialogs (redesign-admin-console-records D3): administrators, Group members, role mappings,
// service principals and the deny list.
import { useState } from 'react';
import { api } from '../auth.ts';
import {
  ADMIN_ROLES, ask, Chips, Dialog, Field, Picker, Records, refreshKnown, Tabs, time, useBootstrap, useDialog, useGroups, useIdpRoles, useLoad, useTab,
  useUsers, list, notify,
} from '../ui.tsx';

const I = '/api/v1/admin/identity';
const TABS = [
  { id: 'admins', label: 'Administrators' },
  { id: 'members', label: 'Group members' },
  { id: 'mappings', label: 'Role mappings' },
  { id: 'principals', label: 'Service principals' },
  { id: 'deny', label: 'Deny list' },
];

export function Identity() {
  const [tab, setTab] = useTab('identity', TABS.map((t) => t.id));
  return (
    <>
      <Tabs tabs={TABS} value={tab} onChange={setTab} />
      {tab === 'admins' && <Admins />}
      {tab === 'members' && <Members />}
      {tab === 'mappings' && <Mappings />}
      {tab === 'principals' && <Principals />}
      {tab === 'deny' && <DenyList />}
    </>
  );
}

// ---- administrators --------------------------------------------------------------------------------------------------

function Admins() {
  const roles = useLoad<{ user_name: string; role: string }[]>(`${I}/admin-roles`);
  const bootstrap = useBootstrap();
  const users = useUsers();
  const [dlg, open, close] = useDialog<{ user?: string; roles: string[] }>();
  const byUser = new Map<string, string[]>();
  for (const b of bootstrap) byUser.set(b, []);
  for (const r of roles.data ?? []) byUser.set(r.user_name, [...(byUser.get(r.user_name) ?? []), r.role]);
  const rows = roles.data ? [...byUser.entries()].map(([user, rs]) => ({ user, roles: rs, bootstrap: bootstrap.some((b) => b.toLowerCase() === user.toLowerCase()) })).sort((a, b) => a.user.localeCompare(b.user)) : null;

  const save = async (user: string, before: string[], after: string[]) => {
    for (const role of after.filter((r) => !before.includes(r))) await api('POST', `${I}/admin-roles`, { user_name: user, role });
    for (const role of before.filter((r) => !after.includes(r))) await api('DELETE', `${I}/admin-roles`, { user_name: user, role });
    roles.reload();
    refreshKnown();
  };
  return (
    <>
      <Records
        title="Administrators"
        id="admin-roles"
        sub="People who may open this console, and what each role allows."
        items={rows}
        error={roles.error}
        text={(r) => `${r.user} ${r.roles.join(' ')}`}
        head={<tr><th>User</th><th>Roles</th><th /></tr>}
        add={<button className="primary" data-testid="role-grant-open" onClick={() => open({ roles: [] })}>Grant a role</button>}
        empty="Only the bootstrap administrators of the server configuration."
        row={(r) => (
          <tr key={r.user} data-testid={`admin-${r.user}`}>
            <td><b>{r.user}</b></td>
            <td><Chips values={[...(r.bootstrap ? ['platform-admin (bootstrap)'] : []), ...r.roles]} /></td>
            <td className="acts">
              <button className="link" onClick={() => open({ user: r.user, roles: r.roles })}>Edit roles</button>
              {r.roles.length > 0 && (
                <button className="link danger" onClick={async () => {
                  if ((await ask({ title: `Revoke every role of ${r.user}?`, body: <p>{r.user} loses {r.roles.join(', ')}{r.bootstrap ? ' but stays platform-admin through the server configuration' : ' and can no longer open the console'}.</p>, action: 'Revoke roles' })) === null) return;
                  await save(r.user, r.roles, []);
                  notify(`Revoked the roles of ${r.user}.`, 'identity-note');
                }}>Revoke all</button>
              )}
            </td>
          </tr>
        )}
      />
      {dlg && (
        <Dialog
          id="role-grant"
          title={dlg.user ? `Roles of ${dlg.user}` : 'Grant an administrator role'}
          submit={dlg.user ? 'Save roles' : 'Grant'}
          onClose={close}
          noteId="identity-note"
          onSubmit={async (f) => {
            const user = dlg.user ?? f.user ?? '';
            if (!user) throw new Error('Choose a user.');
            const chosen = ADMIN_ROLES.map((x) => x.role).filter((x) => f[`role.${x}`] === 'on');
            await save(user, dlg.roles, chosen);
            return dlg.user ? `Roles of ${user} saved.` : `Granted ${chosen.join(', ') || 'nothing'} to ${user}.`;
          }}
        >
          {!dlg.user && (
            <Field label="User" help="People who signed in, belong to a Group or hold a role are listed. A new name takes effect when they sign in.">
              <Picker name="user" options={users} allowNew newHint="not seen yet" placeholder="Search people" required testid="role-user" label="User" />
            </Field>
          )}
          <fieldset className="choices">
            <legend>Roles</legend>
            {ADMIN_ROLES.map((x) => (
              <label key={x.role}>
                <input type="checkbox" name={`role.${x.role}`} defaultChecked={dlg.roles.includes(x.role)} />
                <span>{x.role}</span>
                <small>{x.what}</small>
              </label>
            ))}
          </fieldset>
        </Dialog>
      )}
    </>
  );
}

// ---- Group members ---------------------------------------------------------------------------------------------------

function Members() {
  const members = useLoad<{ user_name: string; skowork_group: string }[]>(`${I}/group-members`);
  const users = useUsers();
  const groups = useGroups();
  const [dlg, open, close] = useDialog<{ group?: string; users: string[] }>();
  const byGroup = new Map<string, string[]>();
  for (const m of members.data ?? []) byGroup.set(m.skowork_group, [...(byGroup.get(m.skowork_group) ?? []), m.user_name]);
  const rows = members.data ? [...byGroup.entries()].map(([group, us]) => ({ group, users: us })) : null;
  return (
    <>
      <Records
        title="Direct Group members"
        id="group-members"
        sub="Users assigned to a Group here, in addition to the Groups their identity-provider roles map to."
        items={rows}
        error={members.error}
        text={(r) => `${r.group} ${r.users.join(' ')}`}
        head={<tr><th>Group</th><th>Members</th><th /></tr>}
        add={<button className="primary" data-testid="member-add-open" onClick={() => open({ users: [] })}>Add members</button>}
        empty="No user is assigned to a Group directly."
        row={(r) => (
          <tr key={r.group} data-testid={`members-${r.group}`}>
            <td><b>{r.group}</b></td>
            <td><Chips values={r.users} /></td>
            <td className="acts"><button className="link" onClick={() => open({ group: r.group, users: r.users })}>Edit members</button></td>
          </tr>
        )}
      />
      {dlg && (
        <Dialog
          id="member-add"
          title={dlg.group ? `Members of ${dlg.group}` : 'Add users to a Group'}
          submit={dlg.group ? 'Save members' : 'Add'}
          onClose={close}
          noteId="identity-note"
          onSubmit={async (f) => {
            const group = dlg.group ?? f.group ?? '';
            if (!group) throw new Error('Choose a Group.');
            const after = list(f.users);
            const removed = dlg.users.filter((u) => !after.includes(u));
            for (const u of after.filter((x) => !dlg.users.includes(x))) await api('POST', `${I}/group-members`, { user_name: u, skowork_group: group });
            for (const u of removed) await api('DELETE', `${I}/group-members`, { user_name: u, skowork_group: group });
            members.reload();
            refreshKnown();
            return `Members of ${group} saved.`;
          }}
        >
          {!dlg.group && (
            <Field label="Group" help="Lowercase letters, digits, - and _. A new Group can get a policy later.">
              <Picker name="group" options={groups} allowNew newHint="new Group" placeholder="Search Groups" required testid="member-group" label="Group" />
            </Field>
          )}
          <Field label="Members">
            <Picker name="users" options={users} multiple allowNew newHint="not seen yet" defaultValue={dlg.users} placeholder="Search people" testid="member-users" label="Members" />
          </Field>
        </Dialog>
      )}
    </>
  );
}

// ---- role mappings ---------------------------------------------------------------------------------------------------

function Mappings() {
  const maps = useLoad<{ idp_role: string; skowork_group: string }[]>(`${I}/role-mappings`);
  const roles = useIdpRoles();
  const groups = useGroups();
  const [dlg, open, close] = useDialog();
  const put = (mappings: { idp_role: string; skowork_group: string }[], reason: string) => api('PUT', `${I}/role-mappings`, { mappings, reason });
  return (
    <>
      <Records
        title="Role mappings"
        id="role-mappings"
        sub="Users whose token carries an identity-provider role are members of the mapped Group."
        items={maps.data}
        error={maps.error}
        text={(m) => `${m.idp_role} ${m.skowork_group}`}
        head={<tr><th>Identity-provider role</th><th>Group</th><th /></tr>}
        add={<button className="primary" data-testid="mapping-add-open" onClick={() => open(true)}>Add mapping</button>}
        empty="No identity-provider role is mapped to a Group."
        row={(m) => (
          <tr key={m.idp_role + m.skowork_group}>
            <td><code>{m.idp_role}</code></td>
            <td><b>{m.skowork_group}</b></td>
            <td className="acts">
              <button className="link danger" onClick={async () => {
                const reason = await ask({ title: 'Remove this mapping?', body: <p>Users with <code>{m.idp_role}</code> are no longer members of {m.skowork_group}, unless they are assigned directly.</p>, action: 'Remove mapping', reason: 'optional' });
                if (reason === null) return;
                await put(maps.data!.filter((x) => x !== m), reason || 'removed in admin console');
                maps.reload();
                notify('Mapping removed.', 'identity-note');
              }}>Remove</button>
            </td>
          </tr>
        )}
      />
      {dlg && (
        <Dialog id="mapping-add" title="Map an identity-provider role to a Group" submit="Add mapping" onClose={close} noteId="identity-note"
          onSubmit={async (f) => {
            if (!f.role || !f.group) throw new Error('Choose a role and a Group.');
            await put([...(maps.data ?? []), { idp_role: f.role, skowork_group: f.group }], f.reason ?? '');
            maps.reload();
            return `${f.role} now maps to ${f.group}.`;
          }}>
          <div className="two">
            <Field label="Identity-provider role" help="Roles seen in users' tokens are listed.">
              <Picker name="role" options={roles} allowNew newHint="not seen yet" required testid="mapping-role" label="Identity-provider role" />
            </Field>
            <Field label="Group">
              <Picker name="group" options={groups} allowNew newHint="new Group" required testid="mapping-group" label="Group" />
            </Field>
          </div>
          <Field label="Reason (optional)"><input name="reason" /></Field>
        </Dialog>
      )}
    </>
  );
}

// ---- service principals ----------------------------------------------------------------------------------------------

interface Principal { client_id: string; name: string; apis: string[]; registered_by: string; registered_at: string; retired_by: string | null; retired_at: string | null }

function Principals() {
  const principals = useLoad<Principal[]>(`${I}/service-principals`);
  const [dlg, open, close] = useDialog();
  return (
    <>
      <Records
        title="Service principals"
        id="service-principals"
        sub="Systems that call Skowork APIs with client-credentials tokens of the identity provider. Each may call only its APIs."
        items={principals.data}
        error={principals.error}
        text={(p) => `${p.name} ${p.client_id} ${p.apis.join(' ')}`}
        head={<tr><th>Name</th><th>Client id</th><th>APIs</th><th>State</th><th /></tr>}
        add={<button className="primary" data-testid="sp-add-open" onClick={() => open(true)}>Register</button>}
        empty="No system calls Skowork APIs yet."
        row={(p) => (
          <tr key={p.client_id} data-testid={`sp-${p.name}`}>
            <td><b>{p.name}</b></td><td><code>{p.client_id}</code></td><td><Chips values={p.apis} /></td>
            <td>{p.retired_at ? <><span className="state revoked">retired</span><small>{time(p.retired_at)} by {p.retired_by}</small></> : <><span className="state ok">active</span><small>since {time(p.registered_at)}</small></>}</td>
            <td className="acts">
              {!p.retired_at && (
                <button className="link danger" onClick={async () => {
                  if ((await ask({ title: `Retire ${p.name}?`, body: <p>Tokens of <code>{p.client_id}</code> are refused from then on. This cannot be undone; register it again to restore access.</p>, action: 'Retire' })) === null) return;
                  await api('POST', `${I}/service-principals/${encodeURIComponent(p.client_id)}/retire`);
                  principals.reload();
                  notify('Retired. Its tokens are refused from now on.', 'identity-note');
                }}>Retire</button>
              )}
            </td>
          </tr>
        )}
      />
      {dlg && (
        <Dialog id="sp-add" title="Register a service principal" submit="Register" onClose={close} noteId="identity-note"
          onSubmit={async (f) => {
            await api('POST', `${I}/service-principals`, { name: f.name, client_id: f.client_id, apis: [f.api] });
            principals.reload();
            return `Registered ${f.name}.`;
          }}>
          <div className="two">
            <Field label="Name" help="How it appears in rules and the audit log."><input name="name" placeholder="ci-bot" required /></Field>
            <Field label="Identity-provider client id"><input name="client_id" required /></Field>
          </div>
          <Field label="API it may call"><select name="api"><option value="tasks">Remote task dispatch</option></select></Field>
        </Dialog>
      )}
    </>
  );
}

// ---- deny list -------------------------------------------------------------------------------------------------------

interface Denied { id: number; kind: string; value: string; reason: string; created_by: string; created_at: string }

function DenyList() {
  const deny = useLoad<Denied[]>(`${I}/deny-list`);
  const insts = useLoad<{ items: { id: string; user_name: string; machine_name: string }[] }>('/api/v1/admin/fleet/installations?page_size=500');
  const users = useUsers();
  const [dlg, open, close] = useDialog();
  const [kind, setKind] = useState('user');
  const instOptions = (insts.data?.items ?? []).map((i) => ({ value: i.id, label: `${i.machine_name} · ${i.user_name}`, hint: i.id.slice(0, 8) }));
  return (
    <>
      <Records
        title="Deny list"
        id="deny-list"
        sub="Users and installations listed here get no tokens."
        items={deny.data}
        error={deny.error}
        text={(d) => `${d.kind} ${d.value} ${d.reason} ${d.created_by}`}
        head={<tr><th>Denied</th><th>Reason</th><th>By</th><th /></tr>}
        add={<button className="danger" data-testid="deny-add-open" onClick={() => (setKind('user'), open(true))}>Deny access</button>}
        empty="Nobody is denied."
        row={(d) => (
          <tr key={d.id} data-testid={`deny-${d.value}`}>
            <td><b>{d.value}</b><small>{d.kind}</small></td><td>{d.reason}</td><td>{d.created_by}<small>{time(d.created_at)}</small></td>
            <td className="acts">
              <button className="link danger" onClick={async () => {
                if ((await ask({ title: `Lift the denial of ${d.value}?`, body: <p>{d.kind === 'user' ? 'The user' : 'The installation'} gets tokens again{d.kind === 'installation' ? ', and its revocation is lifted' : ''}.</p>, action: 'Lift' })) === null) return;
                await api('DELETE', `${I}/deny-list/${d.id}`);
                deny.reload();
                notify('Lifted.', 'identity-note');
              }}>Lift</button>
            </td>
          </tr>
        )}
      />
      {dlg && (
        <Dialog id="deny-add" title="Deny access" submit="Deny access" danger onClose={close} noteId="identity-note"
          intro={<p>The next token request is refused, and connected clients are signed out.</p>}
          onSubmit={async (f) => {
            if (!f.value) throw new Error(kind === 'user' ? 'Choose a user.' : 'Choose an installation.');
            await api('POST', `${I}/deny-list`, { kind: f.kind, value: f.value, reason: f.reason });
            deny.reload();
            refreshKnown();
            return `Denied ${f.value}. Their next token request is refused.`;
          }}>
          <fieldset className="choices">
            <legend>Deny</legend>
            <label><input type="radio" name="kind" value="user" checked={kind === 'user'} onChange={() => setKind('user')} /><span>A user, on every machine</span></label>
            <label><input type="radio" name="kind" value="installation" checked={kind === 'installation'} onChange={() => setKind('installation')} /><span>One installation</span></label>
          </fieldset>
          {kind === 'user'
            ? <Field label="User"><Picker key="user" name="value" options={users} allowNew newHint="not seen yet" required testid="deny-user" label="User" /></Field>
            : <Field label="Installation"><Picker key="inst" name="value" options={instOptions} required testid="deny-installation" label="Installation" /></Field>}
          <Field label="Reason"><input name="reason" placeholder="Left the organization" /></Field>
        </Dialog>
      )}
    </>
  );
}
