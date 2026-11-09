// MCP catalog administration (control-plane/admin-console MCP Catalog Administration) over control-plane/mcp-catalog:
// the catalog as records; add, edit, enable or disable, assign to Groups, decide discovered tools and remove in
// dialogs (redesign-admin-console-records D3).
import { api } from '../auth.ts';
import { ask, Chips, Dialog, Field, list, notify, Picker, Records, time, useDialog, useGroups, useLoad, useUsers } from '../ui.tsx';

const M = '/api/v1/admin/mcp/servers';
interface Server {
  id: string; name?: string; owner: string; kind: string; url?: string; auth?: string; groups: string[]; enabled: boolean;
  discovery: { status: string; error: string | null; at: string | null };
  tools: { name: string; status: string; present: boolean }[];
}

export function Mcp() {
  const servers = useLoad<Server[]>(M);
  const users = useUsers();
  const groups = useGroups();
  const [edit, openEdit, closeEdit] = useDialog<Server | 'new'>();
  const [tools, openTools, closeTools] = useDialog<string>();
  const toolsOf = servers.data?.find((s) => s.id === tools);

  const toggle = async (s: Server) => {
    if (s.enabled && (await ask({ title: `Disable ${s.id}?`, body: <p>Clients stop using it at their next policy update. Its Groups and tool decisions are kept.</p>, action: 'Disable' })) === null) return;
    await api('PUT', `${M}/${s.id}`, { enabled: !s.enabled });
    servers.reload();
    notify(`${s.id} ${s.enabled ? 'disabled' : 'enabled'}.`, 'mcp-note');
  };
  const remove = async (s: Server) => {
    if ((await ask({ title: `Remove ${s.id} from the catalog?`, body: <p>Clients stop using it, and its tool decisions are lost.</p>, action: 'Remove server' })) === null) return;
    await api('DELETE', `${M}/${s.id}`);
    servers.reload();
    notify(`${s.id} removed.`, 'mcp-note');
  };

  return (
    <>
      <Records
        title="MCP servers"
        id="mcp-servers"
        items={servers.data}
        error={servers.error}
        text={(s) => `${s.id} ${s.name ?? ''} ${s.owner} ${s.url ?? ''} ${s.groups.join(' ')}`}
        head={<tr><th>Server</th><th>Groups</th><th>State</th><th>Discovery</th><th>Tools</th><th /></tr>}
        add={<button className="primary" data-testid="mcp-add-open" onClick={() => openEdit('new')}>Add server</button>}
        empty="The catalog is empty. Add the first remote MCP server."
        row={(s) => {
          const pending = s.tools.filter((t) => t.status === 'pending').length;
          return (
            <tr key={s.id} data-testid={`mcp-${s.id}`}>
              <td><b>{s.id}</b><small>{s.kind === 'remote' ? s.url : s.kind} · owner {s.owner}</small></td>
              <td><Chips values={s.groups} empty="no Group" /></td>
              <td><span className={`state ${s.enabled ? 'enabled' : ''}`} data-testid={`mcp-state-${s.id}`}>{s.enabled ? 'enabled' : 'disabled'}</span></td>
              <td><span className={`state ${s.discovery.status === 'ok' ? 'ok' : s.discovery.status === 'failed' ? 'failing' : ''}`}>{s.discovery.status}</span>{s.discovery.error && <small className="err">{s.discovery.error}</small>}<small>{time(s.discovery.at)}</small></td>
              <td>
                <span className="small">{s.tools.map((t) => t.name).join(', ') || '—'}</span>
                {pending > 0 && <small><span className="state pending">{pending} to decide</span></small>}
              </td>
              <td className="acts">
                <button className="link" data-testid={`mcp-edit-${s.id}`} onClick={() => openEdit(s)}>Edit</button>
                {s.tools.length > 0 && <button className="link" data-testid={`mcp-tools-${s.id}`} onClick={() => openTools(s.id)}>Tools</button>}
                {s.kind === 'remote' && <button className="link" onClick={async () => (await api('POST', `${M}/${s.id}/discover`), servers.reload(), notify('Discovery ran.', 'mcp-note'))}>Discover</button>}
                <button className="link" data-testid={`mcp-toggle-${s.id}`} onClick={() => void toggle(s)}>{s.enabled ? 'Disable' : 'Enable'}</button>
                <button className="link danger" onClick={() => void remove(s)}>Remove</button>
              </td>
            </tr>
          );
        }}
      />

      {edit && (
        <Dialog
          id={edit === 'new' ? 'mcp-add-form' : `mcp-groups-${edit.id}`}
          title={edit === 'new' ? 'Add a remote MCP server' : `Edit ${edit.id}`}
          submit={edit === 'new' ? 'Add and discover' : 'Save'}
          onClose={closeEdit}
          noteId="mcp-note"
          intro={edit === 'new' ? <p>The server is enabled once its tools are discovered. Clients in its Groups get it with their next policy update.</p> : undefined}
          onSubmit={async (f) => {
            if (edit === 'new') {
              await api('POST', M, { id: f.id, owner: f.owner, kind: 'remote', url: f.url, auth: f.auth, groups: list(f.groups) });
              servers.reload();
              return `${f.id} added.`;
            }
            const changes: Record<string, unknown> = { groups: list(f.groups) };
            if (f.owner) changes.owner = f.owner;
            if (edit.kind === 'remote' && f.url !== edit.url) changes.url = f.url;
            if (edit.kind === 'remote' && f.auth !== edit.auth) changes.auth = f.auth;
            await api('PUT', `${M}/${edit.id}`, changes);
            servers.reload();
            return `${edit.id} saved.`;
          }}
        >
          {edit === 'new' && <Field label="Id" help="Lowercase, as clients and policies name it."><input name="id" placeholder="jira-corp" required /></Field>}
          {(edit === 'new' || edit.kind === 'remote') && (
            <div className="two">
              <Field label="URL"><input name="url" type="url" placeholder="https://…/mcp" defaultValue={edit === 'new' ? '' : edit.url} required /></Field>
              <Field label="Authentication">
                <select name="auth" defaultValue={edit === 'new' ? 'user-token' : edit.auth}>
                  <option value="user-token">The user's sign-in token</option><option value="none">None</option>
                </select>
              </Field>
            </div>
          )}
          <Field label="Owner" help="Who answers for the server.">
            <Picker name="owner" options={users} allowNew newHint="not seen yet" defaultValue={edit === 'new' ? '' : edit.owner} required testid="mcp-owner" label="Owner" />
          </Field>
          <Field label="Groups" help="Members of these Groups get the server.">
            <Picker name="groups" options={groups} multiple allowNew newHint="new Group" defaultValue={edit === 'new' ? [] : edit.groups} testid="mcp-groups" label="Groups" />
          </Field>
        </Dialog>
      )}

      {toolsOf && (
        <Dialog id="mcp-tools" title={`Tools of ${toolsOf.id}`} onClose={closeTools} intro={<p>Clients get only the included tools. A tool the server no longer lists is marked as gone.</p>}>
          <table>
            <thead><tr><th>Tool</th><th>Decision</th><th /></tr></thead>
            <tbody>
              {toolsOf.tools.map((t) => (
                <tr key={t.name}>
                  <td><code>{t.name}</code>{!t.present && <small>gone</small>}</td>
                  <td><span className={`state ${t.status === 'included' ? 'ok' : t.status === 'pending' ? 'pending' : ''}`}>{t.status}</span></td>
                  <td className="acts">
                    {t.status !== 'included' && <button type="button" className="link" onClick={async () => (await api('PUT', `${M}/${toolsOf.id}/tools/${encodeURIComponent(t.name)}`, { decision: 'included' }), servers.reload())}>Include</button>}
                    {t.status !== 'excluded' && <button type="button" className="link danger" onClick={async () => (await api('PUT', `${M}/${toolsOf.id}/tools/${encodeURIComponent(t.name)}`, { decision: 'excluded' }), servers.reload())}>Exclude</button>}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </Dialog>
      )}
    </>
  );
}
