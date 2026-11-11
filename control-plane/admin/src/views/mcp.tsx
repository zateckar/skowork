// Connectors administration (control-plane/admin-console Connector Administration, Connector Selection And Risk and
// Personal Access Token Settings In Console) over control-plane/mcp-catalog and control-plane/api-connectors. MCP servers
// keep their dialogs; an API connector has a settings dialog and an operations dialog: upload the OpenAPI file, read the
// report, select operations, set risk classes, and see the control plane's reason next to a refused field or row.
import { useState } from 'react';
import { ApiError, api, token } from '../auth.ts';
import { ask, Chips, Dialog, Field, list, notify, Picker, Records, time, useDialog, useGroups, useLoad, useUsers } from '../ui.tsx';

const C = '/api/v1/admin/connectors';
const M = '/api/v1/admin/mcp/servers';
const RISK = ['read', 'write-local', 'external', 'destructive'];
const AUTH = [
  { value: 'none', label: 'None' },
  { value: 'api-key', label: 'An API key (write-only)' },
  { value: 'user-token', label: "The user's sign-in token" },
  { value: 'user-pat', label: "Each user's own personal access token" },
];

interface Connector {
  id: string; name?: string; owner: string; kind: string; url?: string; base_url?: string; auth?: string;
  key_header?: string; key_scheme?: string; help_text?: string; help_link?: string;
  groups: string[]; enabled: boolean;
  discovery: { status: string; error: string | null; at: string | null };
  tools: { name: string; status: string; present: boolean }[];
  api?: { counts: { operations: number; usable: number; selected: number; flagged: number; gone: number } };
}

interface Operation {
  key: string; method: string; path: string; tool: string | null; description: string; usable: boolean; reason: string | null;
  status: 'new' | 'unselected' | 'selected' | 'gone'; risk: string; risk_reason: string | null;
}

interface Detail extends Connector {
  api: {
    document: { title: string; uploaded_at: string; uploaded_by: string } | null;
    counts: { operations: number; usable: number; selected: number; flagged: number; gone: number };
    operations: Operation[];
    key: { set: boolean; hint?: string; set_at?: string };
    token_holders: number;
  };
}

/** A request whose body is a document as text (the OpenAPI file); api() sends JSON only. */
async function putText(path: string, text: string): Promise<unknown> {
  const r = await fetch(path, { method: 'PUT', headers: { authorization: `Bearer ${await token()}`, 'content-type': 'text/plain' }, body: text });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new ApiError(r.status, j?.error?.code ?? `HTTP_${r.status}`, j?.error?.message ?? r.statusText, j?.error ?? {});
  return j;
}

/** The control plane's reason for a refused field, shown next to it (Connector Selection And Risk). */
function fieldError(err: unknown, field: string): string | null {
  return err instanceof ApiError && err.details.field === field ? err.message : null;
}

export function Mcp() {
  const servers = useLoad<Connector[]>(C);
  const users = useUsers();
  const groups = useGroups();
  const [edit, openEdit, closeEdit] = useDialog<Connector | 'new'>();
  const [apiNew, openApiNew, closeApiNew] = useDialog<true>();
  const [ops, openOps, closeOps] = useDialog<string>();
  const [tools, openTools, closeTools] = useDialog<string>();
  const toolsOf = servers.data?.find((s) => s.id === tools);

  const toggle = async (s: Connector) => {
    if (s.enabled && (await ask({ title: `Disable ${s.id}?`, body: <p>Clients stop using it at their next policy update. Its Groups and decisions are kept.</p>, action: 'Disable' })) === null) return;
    await api('PUT', `${M}/${s.id}`, { enabled: !s.enabled });
    servers.reload();
    notify(`${s.id} ${s.enabled ? 'disabled' : 'enabled'}.`, 'mcp-note');
  };
  const remove = async (s: Connector) => {
    if ((await ask({ title: `Remove ${s.id} from the catalog?`, body: <p>Clients stop using it, its decisions are lost, and every personal access token of it is deleted.</p>, action: 'Remove connector' })) === null) return;
    await api('DELETE', s.kind === 'api' ? `${C}/${s.id}` : `${M}/${s.id}`);
    servers.reload();
    notify(`${s.id} removed.`, 'mcp-note');
  };

  return (
    <>
      <Records
        title="Connectors"
        id="connectors"
        items={servers.data}
        error={servers.error}
        text={(s) => `${s.id} ${s.name ?? ''} ${s.kind} ${s.owner} ${s.url ?? s.base_url ?? ''} ${s.groups.join(' ')}`}
        head={<tr><th>Connector</th><th>Groups</th><th>State</th><th>Discovery or operations</th><th>Tools</th><th /></tr>}
        add={
          <>
            <button className="primary" data-testid="mcp-add-open" onClick={() => openEdit('new')}>Add MCP server</button>
            <button className="primary" data-testid="api-add-open" onClick={() => openApiNew(true)}>Add API connector</button>
          </>
        }
        empty="No connectors yet. Add an MCP server or an API connector."
        row={(s) => {
          const isApi = s.kind === 'api';
          const pending = s.tools.filter((t) => t.status === 'pending').length;
          return (
            <tr key={s.id} data-testid={`mcp-${s.id}`}>
              <td>
                <b>{s.id}</b>
                <small>{isApi ? `API connector · ${s.base_url}` : s.kind === 'remote' ? `MCP server · ${s.url}` : 'MCP server · local'} · owner {s.owner}</small>
              </td>
              <td><Chips values={s.groups} empty="no Group" /></td>
              <td><span className={`state ${s.enabled ? 'enabled' : ''}`} data-testid={`mcp-state-${s.id}`}>{s.enabled ? 'enabled' : 'disabled'}</span></td>
              <td>
                {isApi ? (
                  <>
                    <span className="small">{s.api?.counts.usable ?? 0} usable of {s.api?.counts.operations ?? 0} operations</span>
                    {(s.api?.counts.flagged ?? 0) > 0 && <small><span className="state pending">{s.api?.counts.flagged} to review</span></small>}
                  </>
                ) : (
                  <>
                    <span className={`state ${s.discovery.status === 'ok' ? 'ok' : s.discovery.status === 'failed' ? 'failing' : ''}`}>{s.discovery.status}</span>
                    {s.discovery.error && <small className="err">{s.discovery.error}</small>}
                    <small>{time(s.discovery.at)}</small>
                  </>
                )}
              </td>
              <td>
                {isApi ? (
                  <span className="small">{s.api?.counts.selected ?? 0} selected</span>
                ) : (
                  <>
                    <span className="small">{s.tools.map((t) => t.name).join(', ') || '—'}</span>
                    {pending > 0 && <small><span className="state pending">{pending} to decide</span></small>}
                  </>
                )}
              </td>
              <td className="acts">
                {isApi && <button className="link" data-testid={`api-ops-${s.id}`} onClick={() => openOps(s.id)}>Operations</button>}
                <button className="link" data-testid={`mcp-edit-${s.id}`} onClick={() => openEdit(s)}>Edit</button>
                {!isApi && s.tools.length > 0 && <button className="link" data-testid={`mcp-tools-${s.id}`} onClick={() => openTools(s.id)}>Tools</button>}
                {s.kind === 'remote' && <button className="link" onClick={async () => (await api('POST', `${M}/${s.id}/discover`), servers.reload(), notify('Discovery ran.', 'mcp-note'))}>Discover</button>}
                <button className="link" data-testid={`mcp-toggle-${s.id}`} onClick={() => void toggle(s)}>{s.enabled ? 'Disable' : 'Enable'}</button>
                <button className="link danger" onClick={() => void remove(s)}>Remove</button>
              </td>
            </tr>
          );
        }}
      />

      {edit && edit !== 'new' && edit.kind === 'api' && (
        <ApiSettingsDialog connector={edit} users={users} groups={groups} onDone={() => servers.reload()} onClose={closeEdit} />
      )}

      {apiNew && <ApiSettingsDialog connector={null} users={users} groups={groups} onDone={() => servers.reload()} onClose={closeApiNew} />}

      {edit && (edit === 'new' || edit.kind !== 'api') && (
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

      {ops && <ApiOperationsDialog id={ops} onDone={() => servers.reload()} onClose={closeOps} />}

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

/** Settings of an API connector: base URL, authentication, the write-only key, and the personal token settings. */
function ApiSettingsDialog({ connector, users, groups, onDone, onClose }: {
  connector: Connector | null;
  users: ReturnType<typeof useUsers>;
  groups: ReturnType<typeof useGroups>;
  onDone: () => void;
  onClose: () => void;
}) {
  const [auth, setAuth] = useState(connector?.auth ?? 'none');
  const [errs, setErrs] = useState<Record<string, string>>({});
  const fieldErrs = (err: unknown) => {
    const next: Record<string, string> = {};
    for (const f of ['base_url', 'auth', 'key_header', 'key_scheme', 'help_text', 'help_link', 'api_key', 'groups', 'id']) {
      const m = fieldError(err, f);
      if (m) next[f] = m;
    }
    setErrs(next);
  };
  return (
    <Dialog
      id={connector ? `api-settings-${connector.id}` : 'api-add-form'}
      title={connector ? `Edit ${connector.id}` : 'Add an API connector'}
      submit={connector ? 'Save' : 'Add connector'}
      onClose={onClose}
      noteId="mcp-note"
      intro={connector ? undefined : <p>Upload its OpenAPI document after adding it. Nothing is exposed until an operation is selected.</p>}
      onSubmit={async (f) => {
        const body: Record<string, unknown> = {
          owner: f.owner,
          name: f.name || undefined,
          base_url: f.base_url,
          auth: f.auth,
          key_header: f.key_header || undefined,
          key_scheme: f.key_scheme || undefined,
          help_text: f.help_text || undefined,
          help_link: f.help_link || undefined,
          groups: list(f.groups),
        };
        if (f.api_key) body.api_key = f.api_key;
        try {
          if (connector) await api('PUT', `${C}/${connector.id}/settings`, body);
          else await api('POST', C, { ...body, id: f.id, kind: 'api' });
        } catch (err) {
          fieldErrs(err);
          throw err;
        }
        setErrs({});
        onDone();
        return `${connector?.id ?? f.id} saved.`;
      }}
    >
      {!connector && <Field label="Id" help="Lowercase, as clients and policies name it."><input name="id" placeholder="orders-api" required />{errs.id && <small className="err">{errs.id}</small>}</Field>}
      <div className="two">
        <Field label="Name"><input name="name" defaultValue={connector?.name ?? ''} placeholder="Orders" /></Field>
        <Field label="Base URL" help="HTTPS. The control plane calls only this address."><input name="base_url" type="url" defaultValue={connector?.base_url ?? ''} placeholder="https://orders.internal.example/api" required />{errs.base_url && <small className="err">{errs.base_url}</small>}</Field>
      </div>
      <div className="two">
        <Field label="Authentication">
          <select name="auth" value={auth} onChange={(e) => setAuth(e.target.value)}>
            {AUTH.map((a) => <option key={a.value} value={a.value}>{a.label}</option>)}
          </select>
          {errs.auth && <small className="err">{errs.auth}</small>}
        </Field>
        {(auth === 'api-key' || auth === 'user-pat') && (
          <Field label="Header" help={auth === 'user-pat' ? 'Carries each user’s token.' : 'Carries the key.'}>
            <input name="key_header" defaultValue={connector?.key_header ?? 'Authorization'} placeholder="Authorization" required />
            {errs.key_header && <small className="err">{errs.key_header}</small>}
          </Field>
        )}
      </div>
      {auth === 'api-key' && (
        <Field label="API key" help={connector?.api ? 'Write-only. Leave empty to keep the stored key.' : 'Write-only; it is never shown again.'}>
          <input name="api_key" type="password" autoComplete="new-password" required={!connector} />
          {errs.api_key && <small className="err">{errs.api_key}</small>}
        </Field>
      )}
      {auth === 'user-pat' && (
        <>
          <div className="two">
            <Field label="Scheme" help="Written before the token, for example Bearer. Empty for a bare token.">
              <input name="key_scheme" defaultValue={connector?.key_scheme ?? 'Bearer'} placeholder="Bearer" />
              {errs.key_scheme && <small className="err">{errs.key_scheme}</small>}
            </Field>
            <Field label="Help link" help="Where users create a token. HTTPS only.">
              <input name="help_link" type="url" defaultValue={connector?.help_link ?? ''} placeholder="https://…/tokens" />
              {errs.help_link && <small className="err">{errs.help_link}</small>}
            </Field>
          </div>
          <Field label="Help text" help="20 to 500 characters: where users create their token. Shown to users who have none.">
            <textarea name="help_text" defaultValue={connector?.help_text ?? ''} rows={2} required />
            {errs.help_text && <small className="err">{errs.help_text}</small>}
          </Field>
        </>
      )}
      <Field label="Owner" help="Who answers for the connector.">
        <Picker name="owner" options={users} allowNew newHint="not seen yet" defaultValue={connector?.owner ?? ''} required testid="api-owner" label="Owner" />
      </Field>
      <Field label="Groups" help="Members of these Groups get the connector.">
        <Picker name="groups" options={groups} multiple allowNew newHint="new Group" defaultValue={connector?.groups ?? []} testid="api-groups" label="Groups" />
        {errs.groups && <small className="err">{errs.groups}</small>}
      </Field>
    </Dialog>
  );
}

/** The operations of an API connector: upload its document, read the report, select operations and set their risk. */
function ApiOperationsDialog({ id, onDone, onClose }: { id: string; onDone: () => void; onClose: () => void }) {
  const detail = useLoad<Detail>(`${C}/${id}`);
  const [sel, setSel] = useState<Record<string, { selected: boolean; risk: string; reason: string }>>({});
  const [rowErr, setRowErr] = useState<{ key?: string; text: string } | null>(null);
  const [busy, setBusy] = useState(false);
  const [uploadErr, setUploadErr] = useState<string | null>(null);
  const current = (op: Operation) => sel[op.key] ?? { selected: op.status === 'selected', risk: op.risk, reason: op.risk_reason ?? '' };
  const set = (op: Operation, patch: Partial<{ selected: boolean; risk: string; reason: string }>) => setSel((s) => ({ ...s, [op.key]: { ...current(op), ...patch } }));

  const upload = async (file: File) => {
    setUploadErr(null);
    try {
      await putText(`${C}/${id}/document`, await file.text());
      setSel({});
      detail.reload();
      onDone();
      notify(`The document of ${id} was read.`, 'mcp-note');
    } catch (err) {
      setUploadErr((err as Error).message);
    }
  };
  const save = async () => {
    const d = detail.data!;
    setBusy(true);
    setRowErr(null);
    try {
      const operations = d.api.operations.map((op) => {
        const c = current(op);
        return { key: op.key, selected: c.selected && op.usable, risk: c.risk, risk_reason: c.reason || undefined };
      });
      await api('PUT', `${C}/${id}/operations`, { operations });
      setSel({});
      detail.reload();
      onDone();
      notify(`The selection of ${id} is saved.`, 'mcp-note');
    } catch (err) {
      // The refusal names the operation in its message; it is shown next to that row, and the dialog stays open.
      const message = (err as Error).message;
      const key = detail.data?.api.operations.find((op) => message.startsWith(op.key))?.key;
      setRowErr({ key, text: message });
      throw err;
    } finally {
      setBusy(false);
    }
  };

  const d = detail.data;
  return (
    <Dialog
      id={`api-ops-${id}`}
      wide
      title={`Operations of ${id}`}
      submit="Save selection"
      onClose={onClose}
      onSubmit={save}
      intro={
        <p>
          {d?.api.document ? <>Document <b>{d.api.document.title}</b>, read {time(d.api.document.uploaded_at)}. </> : 'No document yet. '}
          Only operations with a correct description can be selected. Descriptions come from the document and cannot be edited here.
        </p>
      }
    >
      <Field label="OpenAPI document" help="One JSON or YAML file, OpenAPI 3.0.x or 3.1.x. Swagger 2.0 is refused.">
        <input type="file" accept=".json,.yaml,.yml,application/json,application/yaml,text/yaml" data-testid="api-document-file" onChange={(e) => e.target.files?.[0] && void upload(e.target.files[0])} />
        {uploadErr && <small className="err">{uploadErr}</small>}
      </Field>
      {detail.error && <small className="err">{detail.error}</small>}
      {d && (
        <>
          <p className="small">
            {d.api.counts.usable} usable of {d.api.counts.operations} operations · {d.api.counts.selected} selected
            {d.api.counts.flagged > 0 && <> · <span className="state pending">{d.api.counts.flagged} to review</span></>}
            {d.api.counts.gone > 0 && <> · {d.api.counts.gone} gone</>}
          </p>
          {rowErr && !rowErr.key && <p className="err" data-testid="api-ops-error">{rowErr.text}</p>}
          <table data-testid="api-ops-table">
            <thead><tr><th>Use</th><th>Operation</th><th>Description</th><th>Risk</th><th /></tr></thead>
            <tbody>
              {d.api.operations.map((op) => {
                const c = current(op);
                const mine = rowErr?.key === op.key;
                return (
                  <tr key={op.key} data-testid={`api-op-${op.key}`}>
                    <td><input type="checkbox" aria-label={`Use ${op.key}`} disabled={!op.usable || op.status === 'gone'} checked={c.selected && op.usable} onChange={(e) => set(op, { selected: e.target.checked })} /></td>
                    <td>
                      <code>{op.key}</code>
                      {op.tool && <small>tool {op.tool}</small>}
                      {op.status === 'new' && <small><span className="state pending">to review</span></small>}
                      {op.status === 'gone' && <small><span className="state failing">gone</span></small>}
                    </td>
                    <td className="small">{op.description}</td>
                    <td>
                      <select aria-label={`Risk of ${op.key}`} value={c.risk} disabled={!op.usable} onChange={(e) => set(op, { risk: e.target.value })}>
                        {RISK.map((r) => <option key={r} value={r}>{r}</option>)}
                      </select>
                      {c.risk === 'read' && op.method !== 'GET' && (
                        <input aria-label={`Reason for ${op.key}`} placeholder="Why this is only a read" value={c.reason} onChange={(e) => set(op, { reason: e.target.value })} />
                      )}
                    </td>
                    <td>
                      {!op.usable && <span className="small err">{op.reason}</span>}
                      {mine && <small className="err">{rowErr?.text}</small>}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
          {busy && <small>Saving…</small>}
        </>
      )}
    </Dialog>
  );
}
