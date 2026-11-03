// Identity administration (control-plane/admin-console Identity Administration) over control-plane/identity: the deny
// list, service principals and the APIs each may call, Group role mappings and direct assignments, and admin roles.
import { api } from '../auth.ts';
import { Card, confirmed, Err, Field, Form, Note, time, useAction, useLoad } from '../ui.tsx';

const I = '/api/v1/admin/identity';
const ROLES = ['platform-admin', 'security-auditor', 'group-owner', 'skill-reviewer', 'works-council-reviewer'];

export function Identity() {
  const deny = useLoad<{ id: number; kind: string; value: string; reason: string; created_by: string; created_at: string }[]>(`${I}/deny-list`);
  const maps = useLoad<{ idp_role: string; workbench_group: string }[]>(`${I}/role-mappings`);
  const members = useLoad<{ user_name: string; workbench_group: string }[]>(`${I}/group-members`);
  const roles = useLoad<{ user_name: string; role: string }[]>(`${I}/admin-roles`);
  const principals = useLoad<{ client_id: string; name: string; apis: string[]; registered_by: string; registered_at: string; retired_by: string | null; retired_at: string | null }[]>(
    `${I}/service-principals`,
  );
  const act = useAction();
  const run = (fn: () => Promise<unknown>, after: () => void, note: string) => act.run(async () => (await fn(), after()), note);
  return (
    <div className="grid">
      <Card title="Deny list" id="deny-list">
        <p className="muted">Users and installations listed here get no tokens.</p>
        <table>
          <thead><tr><th>Kind</th><th>Value</th><th>Reason</th><th>By</th><th>At</th><th /></tr></thead>
          <tbody>
            {deny.data?.map((d) => (
              <tr key={d.id} data-testid={`deny-${d.value}`}>
                <td>{d.kind}</td><td>{d.value}</td><td>{d.reason}</td><td>{d.created_by}</td><td>{time(d.created_at)}</td>
                <td><button onClick={() => confirmed(`Lift the denial of ${d.value}?`) && void run(() => api('DELETE', `${I}/deny-list/${d.id}`), deny.reload, 'Lifted.')}>Lift</button></td>
              </tr>
            ))}
            {deny.data?.length === 0 && <tr><td colSpan={6} className="empty">Nobody is denied.</td></tr>}
          </tbody>
        </table>
        <Form id="deny-add" onSubmit={(f) => run(() => api('POST', `${I}/deny-list`, { kind: f.kind, value: f.value, reason: f.reason }), deny.reload, 'Denied. Their next token request is refused.')}>
          <Field label="Kind"><select name="kind"><option value="user">user</option><option value="installation">installation</option></select></Field>
          <Field label="User name or installation id" grow><input name="value" required /></Field>
          <Field label="Reason" grow><input name="reason" /></Field>
          <button>Deny</button>
        </Form>
      </Card>

      <Card title="Service principals" id="service-principals">
        <p className="muted">Systems that call Workbench APIs with client-credentials tokens of the identity provider. Each may call only the APIs listed here.</p>
        <table>
          <thead><tr><th>Name</th><th>Client id</th><th>APIs</th><th>Registered</th><th /></tr></thead>
          <tbody>
            {principals.data?.map((p) => (
              <tr key={p.client_id} data-testid={`sp-${p.name}`}>
                <td>{p.name}</td><td>{p.client_id}</td><td>{p.apis.join(', ')}</td>
                <td>{p.retired_at ? `retired ${time(p.retired_at)} by ${p.retired_by}` : `${time(p.registered_at)} by ${p.registered_by}`}</td>
                <td>{!p.retired_at && <button onClick={() => confirmed(`Retire ${p.name}? Its tokens are refused from then on.`) && void run(() => api('POST', `${I}/service-principals/${encodeURIComponent(p.client_id)}/retire`), principals.reload, 'Retired. Its tokens are refused from now on.')}>Retire</button>}</td>
              </tr>
            ))}
            {principals.data?.length === 0 && <tr><td colSpan={5} className="empty">No service principals.</td></tr>}
          </tbody>
        </table>
        <Form id="sp-add" onSubmit={(f) => run(() => api('POST', `${I}/service-principals`, { name: f.name, client_id: f.client_id, apis: [f.api] }), principals.reload, 'Registered.')}>
          <Field label="Name"><input name="name" placeholder="ci" required /></Field>
          <Field label="Identity-provider client id" grow><input name="client_id" required /></Field>
          <Field label="API"><select name="api"><option value="tasks">tasks (Remote task dispatch)</option></select></Field>
          <button>Register</button>
        </Form>
      </Card>

      <Card title="Group role mappings" id="role-mappings">
        <p className="muted">Users with an identity-provider role are members of the mapped Group.</p>
        <ul className="entries">
          {maps.data?.map((m) => (
            <li key={m.idp_role + m.workbench_group}>
              <span>{m.idp_role} → {m.workbench_group}</span>
              <button className="icon" aria-label={`Remove mapping ${m.idp_role} to ${m.workbench_group}`} title="Remove"
                onClick={() => confirmed(`Remove the mapping ${m.idp_role} → ${m.workbench_group}?`) && void run(() => api('PUT', `${I}/role-mappings`, { mappings: maps.data!.filter((x) => x !== m), reason: 'removed in admin console' }), maps.reload, 'Mapping removed.')}>×</button>
            </li>
          ))}
          {maps.data?.length === 0 && <li className="empty">No mappings.</li>}
        </ul>
        <Form id="mapping-add" onSubmit={(f) => run(() => api('PUT', `${I}/role-mappings`, { mappings: [...(maps.data ?? []), { idp_role: f.role, workbench_group: f.group }], reason: f.reason }), maps.reload, 'Mapping added.')}>
          <Field label="Identity-provider role" grow><input name="role" required /></Field>
          <Field label="Group"><input name="group" required /></Field>
          <Field label="Reason" grow><input name="reason" /></Field>
          <button>Add</button>
        </Form>
      </Card>

      <Card title="Direct Group assignments" id="group-members">
        <ul className="entries">
          {members.data?.map((m) => (
            <li key={m.user_name + m.workbench_group}>
              <span>{m.workbench_group}: {m.user_name}</span>
              <button className="icon" aria-label={`Remove ${m.user_name} from ${m.workbench_group}`} title="Remove"
                onClick={() => confirmed(`Remove ${m.user_name} from ${m.workbench_group}?`) && void run(() => api('DELETE', `${I}/group-members`, m), members.reload, 'Removed.')}>×</button>
            </li>
          ))}
          {members.data?.length === 0 && <li className="empty">No direct assignments.</li>}
        </ul>
        <Form id="member-add" onSubmit={(f) => run(() => api('POST', `${I}/group-members`, { user_name: f.user, workbench_group: f.group }), members.reload, 'Assigned.')}>
          <Field label="User name" grow><input name="user" required /></Field>
          <Field label="Group" grow><input name="group" required /></Field>
          <button>Assign</button>
        </Form>
      </Card>

      <Card title="Administrator roles" id="admin-roles">
        <ul className="entries">
          {roles.data?.map((r) => (
            <li key={r.user_name + r.role}>
              <span>{r.user_name}: {r.role}</span>
              <button className="icon" aria-label={`Revoke ${r.role} from ${r.user_name}`} title="Revoke"
                onClick={() => confirmed(`Revoke ${r.role} from ${r.user_name}?`) && void run(() => api('DELETE', `${I}/admin-roles`, r), roles.reload, 'Revoked.')}>×</button>
            </li>
          ))}
          {roles.data?.length === 0 && <li className="empty">Only bootstrap administrators.</li>}
        </ul>
        <Form id="role-grant" onSubmit={(f) => run(() => api('POST', `${I}/admin-roles`, { user_name: f.user, role: f.role }), roles.reload, 'Granted.')}>
          <Field label="User name" grow><input name="user" required /></Field>
          <Field label="Role"><select name="role">{ROLES.map((r) => <option key={r}>{r}</option>)}</select></Field>
          <button>Grant</button>
        </Form>
      </Card>
      <Err error={act.error || deny.error || maps.error} id="identity-error" />
      <Note text={act.note} id="identity-note" />
    </div>
  );
}
