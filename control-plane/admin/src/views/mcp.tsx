// MCP catalog administration (control-plane/admin-console MCP Catalog Administration) over control-plane/mcp-catalog:
// add, update, enable or disable, assign to Groups, review discovered tools, remove.
import { api } from '../auth.ts';
import { Card, confirmed, Err, Field, Form, Note, time, useAction, useLoad } from '../ui.tsx';

const M = '/api/v1/admin/mcp/servers';
interface Server {
  id: string; owner: string; kind: string; url?: string; groups: string[]; enabled: boolean;
  discovery: { status: string; error: string | null; at: string | null };
  tools: { name: string; status: string; present: boolean }[];
}
const list = (s: string | undefined) => (s ?? '').split(',').map((x) => x.trim()).filter(Boolean);

export function Mcp() {
  const servers = useLoad<Server[]>(M);
  const act = useAction();
  const run = (fn: () => Promise<unknown>, note: string) => act.run(async () => (await fn(), servers.reload()), note);
  return (
    <>
      <Card title="MCP servers" id="mcp-servers">
        <table>
          <thead><tr><th>Server</th><th>Groups</th><th>State</th><th>Discovery</th><th>Tools</th><th /></tr></thead>
          <tbody>
            {servers.data?.map((s) => (
              <tr key={s.id} data-testid={`mcp-${s.id}`}>
                <td><b>{s.id}</b><br /><small>{s.kind} {s.url ?? ''} · {s.owner}</small></td>
                <td>
                  <Form id={`mcp-groups-${s.id}`} onSubmit={(f) => run(() => api('PUT', `${M}/${s.id}`, { groups: list(f.groups) }), `Groups of ${s.id} saved.`)}>
                    <input name="groups" aria-label={`Groups of ${s.id}`} defaultValue={s.groups.join(', ')} /> <button>Assign</button>
                  </Form>
                </td>
                <td data-testid={`mcp-state-${s.id}`}>{s.enabled ? 'enabled' : 'disabled'}</td>
                <td>{s.discovery.status}{s.discovery.error ? `: ${s.discovery.error}` : ''} <small>{time(s.discovery.at)}</small></td>
                <td>
                  {s.tools.map((t) => (
                    <div key={t.name}>
                      {t.name} <small>{t.status}{t.present ? '' : ', gone'}</small>
                      {t.status === 'pending' && (
                        <>
                          {' '}<button onClick={() => void run(() => api('PUT', `${M}/${s.id}/tools/${encodeURIComponent(t.name)}`, { decision: 'included' }), `${t.name} included.`)}>Include</button>
                          <button onClick={() => void run(() => api('PUT', `${M}/${s.id}/tools/${encodeURIComponent(t.name)}`, { decision: 'excluded' }), `${t.name} excluded.`)}>Exclude</button>
                        </>
                      )}
                    </div>
                  ))}
                </td>
                <td className="row">
                  <button data-testid={`mcp-toggle-${s.id}`} onClick={() => (!s.enabled || confirmed(`Disable ${s.id}? Clients stop using it.`)) && void run(() => api('PUT', `${M}/${s.id}`, { enabled: !s.enabled }), `${s.id} ${s.enabled ? 'disabled' : 'enabled'}.`)}>
                    {s.enabled ? 'Disable' : 'Enable'}
                  </button>
                  {s.kind === 'remote' && <button onClick={() => void run(() => api('POST', `${M}/${s.id}/discover`), 'Discovery ran.')}>Discover</button>}
                  <button className="danger" onClick={() => confirmed(`Remove ${s.id} from the catalog? Clients stop using it.`) && void run(() => api('DELETE', `${M}/${s.id}`), `${s.id} removed.`)}>Remove</button>
                </td>
              </tr>
            ))}
            {servers.data?.length === 0 && <tr><td colSpan={6} className="empty">The catalog is empty.</td></tr>}
          </tbody>
        </table>
      </Card>
      <Card title="Add a remote MCP server" id="mcp-add">
        <Form id="mcp-add-form" onSubmit={(f) => run(() => api('POST', M, { id: f.id, owner: f.owner, kind: 'remote', url: f.url, auth: f.auth, groups: list(f.groups) }), `${f.id} added.`)}>
          <Field label="Id"><input name="id" placeholder="jira-corp" required /></Field>
          <Field label="Owner"><input name="owner" required /></Field>
          <Field label="URL" grow><input name="url" placeholder="https://…/mcp" required /></Field>
          <Field label="Authentication"><select name="auth"><option value="user-token">user's sign-in token</option><option value="none">none</option></select></Field>
          <Field label="Groups (comma-separated)" grow><input name="groups" /></Field>
          <button>Add</button>
        </Form>
      </Card>
      <Err error={act.error || servers.error} id="mcp-error" />
      <Note text={act.note} id="mcp-note" />
    </>
  );
}
