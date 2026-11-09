// LLM connections (control-plane/admin-console LLM Connection Administration): the OpenAI-compatible upstreams of
// control-plane/llm-gateway as records with their status; each opens to its discovered models. Adding and editing a
// connection, and the capabilities of a model, happen in dialogs. Credentials are write-only
// (redesign-admin-console-records D3).
import { Fragment, useState } from 'react';
import { api } from '../auth.ts';
import { ask, Dialog, Field, notify, Records, time, useDialog, useLoad } from '../ui.tsx';

interface Model {
  id: string;
  kind: 'chat' | 'embed' | 'rerank' | 'image';
  context_window: number;
  max_output_tokens?: number;
  tools: boolean;
  images: boolean;
  reasoning: boolean;
  dimension?: number;
  last_seen: string | null;
  missing: boolean;
}

interface Connection {
  id: number;
  name: string;
  base_url: string;
  credential_set: boolean;
  credential_hint: string;
  enabled: boolean;
  max_concurrent: number;
  position: number;
  status: string;
  status_detail: string | null;
  checked_at: string | null;
  models: Model[];
}

interface Use { scope: string; role: string; model: string }

const BASE = '/api/v1/admin/llm/connections';
const caps = (m: Model) => [m.tools && 'tools', m.images && 'images', m.reasoning && 'reasoning'].filter(Boolean).join(', ') || '—';

export function Connections() {
  const list = useLoad<{ connections: Connection[]; key_available: boolean }>(BASE);
  const [open, setOpen] = useState<number | null>(null);
  const [edit, openEdit, closeEdit] = useDialog<Connection | 'new'>();
  const [model, openModel, closeModel] = useDialog<{ c: Connection; m: Model }>();

  const test = async (c: Connection) => {
    const r = await api<Connection>('POST', `${BASE}/${c.id}/test`);
    list.reload();
    notify(r.status === 'ok' ? `${c.name} answers and lists ${r.models.length} models.` : `Tested ${c.name}: ${r.status}${r.status_detail ? `, ${r.status_detail}` : ''}.`, 'connections-note');
  };
  const toggle = async (c: Connection) => {
    if (c.enabled && (await ask({ title: `Disable ${c.name}?`, body: <p>Requests for its models go to other connections that offer them, or fail.</p>, action: 'Disable' })) === null) return;
    await api('PATCH', `${BASE}/${c.id}`, { enabled: !c.enabled });
    list.reload();
    notify(c.enabled ? `Disabled ${c.name}.` : `Enabled ${c.name}.`, 'connections-note');
  };
  const remove = async (c: Connection) => {
    const uses = (await api<{ uses: Use[] }>('GET', `${BASE}/${c.id}/uses`).catch(() => ({ uses: [] as Use[] }))).uses;
    const ok = await ask({
      title: `Delete ${c.name}?`,
      body: uses.length ? (
        <>
          <p>{c.name} is used by these role mappings:</p>
          <ul>{uses.map((u, i) => <li key={i}>{u.scope.replace(/^group:/, 'Group ')}: {u.role} → {u.model}</li>)}</ul>
          <p>Those roles stop working unless another connection offers the model. Its stored credential is removed.</p>
        </>
      ) : <p>Its stored credential is removed.</p>,
      action: 'Delete connection',
    });
    if (ok === null) return;
    await api('DELETE', `${BASE}/${c.id}${uses.length ? '?confirm=true' : ''}`);
    list.reload();
    notify(`Deleted ${c.name}.`, 'connections-note');
  };

  return (
    <>
      <Records
        title="LLM connections"
        id="connections"
        sub={<>The model servers the gateway forwards to. A role mapping names a model id; it goes to the first enabled connection that offers it, or to one connection when written as <code>connection/model</code>.</>}
        items={list.data?.connections}
        error={list.error}
        filter={false}
        text={(c) => c.name}
        tools={list.data && !list.data.key_available && <span className="state failing" role="alert">WORKBENCH_SECRET_KEY is not set: credentials cannot be stored</span>}
        head={<tr><th>Connection</th><th>Status</th><th>Credential</th><th>Models</th><th>Checked</th><th /></tr>}
        add={<button className="primary" data-testid="connection-add-open" onClick={() => openEdit('new')}>Add connection</button>}
        empty="No connection yet. Add the organization LLM API to give the gateway models."
        row={(c) => (
          <Fragment key={c.id}>
            <tr data-testid={`conn-${c.name}`}>
              <td><b>{c.name}</b><small>{c.base_url}</small></td>
              <td>
                <span className={`state ${c.enabled ? c.status : ''}`} data-testid={`conn-status-${c.name}`}>{c.enabled ? c.status : 'disabled'}</span>
                {c.status_detail && <small className="err">{c.status_detail}</small>}
              </td>
              <td>{c.credential_set ? <code>…{c.credential_hint || '(set)'}</code> : <span className="muted">not set</span>}</td>
              <td>
                <button className="link" aria-expanded={open === c.id} data-testid={`conn-open-${c.name}`} onClick={() => setOpen(open === c.id ? null : c.id)}>
                  {open === c.id ? '▾' : '▸'} {c.models.length} models
                </button>
                {c.max_concurrent ? <small>at most {c.max_concurrent} at once</small> : null}
              </td>
              <td>{time(c.checked_at)}</td>
              <td className="acts">
                <button className="link" data-testid={`conn-edit-open-${c.name}`} onClick={() => openEdit(c)}>Edit</button>
                <button className="link" data-testid={`conn-test-${c.name}`} onClick={() => void test(c)}>Test</button>
                <button className="link" data-testid={`conn-toggle-${c.name}`} onClick={() => void toggle(c)}>{c.enabled ? 'Disable' : 'Enable'}</button>
                <button className="link danger" data-testid={`conn-delete-${c.name}`} onClick={() => void remove(c)}>Delete</button>
              </td>
            </tr>
            {open === c.id && (
              <tr className="detail" data-testid={`conn-detail-${c.name}`}>
                <td colSpan={6}>
                  {c.models.length === 0 ? <p className="empty">No models listed yet. Use Test to ask the server.</p> : (
                    <table>
                      <thead><tr><th>Model</th><th>Kind</th><th>Context window</th><th>Max output</th><th>Capabilities</th><th /></tr></thead>
                      <tbody>
                        {c.models.map((m) => (
                          <tr key={m.id} data-testid={`model-${c.name}-${m.id}`}>
                            <td><code>{m.id}</code>{m.missing && <small><span className="state failing" title="No longer listed by the connection">missing</span></small>}</td>
                            <td>{m.kind}{m.dimension ? <small>{m.dimension} dimensions</small> : null}</td>
                            <td>{m.context_window || '—'}</td><td>{m.max_output_tokens ?? '—'}</td><td>{caps(m)}</td>
                            <td className="acts"><button className="link" data-testid={`model-${c.name}-${m.id}-edit`} onClick={() => openModel({ c, m })}>Edit</button></td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  )}
                </td>
              </tr>
            )}
          </Fragment>
        )}
      />

      {edit && (
        <Dialog
          id={edit === 'new' ? 'connection-add' : `conn-edit-${edit.name}`}
          title={edit === 'new' ? 'Add an LLM connection' : `Edit ${edit.name}`}
          submit={edit === 'new' ? 'Add and test' : 'Save'}
          onClose={closeEdit}
          noteId="connections-note"
          intro={edit === 'new' ? <p>An OpenAI-compatible API. The gateway asks it for its models right away. The credential is stored encrypted and never shown again.</p> : undefined}
          onSubmit={async (f) => {
            const common = { name: f.name, base_url: f.base_url, max_concurrent: Number(f.max_concurrent || 0), ...(f.credential ? { credential: f.credential } : {}) };
            if (edit === 'new') {
              const c = await api<Connection>('POST', BASE, common);
              list.reload();
              setOpen(c.id);
              return c.status === 'ok' ? 'Connection saved and its models listed.' : `Connection saved, but it is ${c.status}${c.status_detail ? `: ${c.status_detail}` : ''}. Fix it with Edit, then Test.`;
            }
            await api('PATCH', `${BASE}/${edit.id}`, { ...common, position: Number(f.position || 0) });
            list.reload();
            return `Saved ${f.name}.`;
          }}
        >
          <div className="two">
            <Field label="Name" help="Lowercase letters, digits and dashes."><input name="name" required pattern="[a-z0-9][a-z0-9-]{0,39}" placeholder="primary" defaultValue={edit === 'new' ? '' : edit.name} autoComplete="off" /></Field>
            <Field label="Base URL"><input name="base_url" required type="url" placeholder="https://llm.example.com/v1" defaultValue={edit === 'new' ? '' : edit.base_url} /></Field>
          </div>
          <Field label={edit === 'new' ? 'Credential (API key)' : 'New credential'} help={edit !== 'new' && edit.credential_set ? `Leave empty to keep the stored one (…${edit.credential_hint}).` : undefined}>
            <input name="credential" type="password" autoComplete="new-password" />
          </Field>
          <div className="two">
            <Field label="Concurrent requests" help="0 means no limit."><input name="max_concurrent" type="number" min={0} defaultValue={edit === 'new' ? 0 : edit.max_concurrent} /></Field>
            {edit !== 'new' && <Field label="Order" help="Lower is asked first for a shared model id."><input name="position" type="number" min={0} defaultValue={edit.position} /></Field>}
          </div>
        </Dialog>
      )}

      {model && (
        <Dialog id={`model-${model.c.name}-${model.m.id}-form`} title={`Capabilities of ${model.m.id}`} submit="Save" onClose={closeModel} noteId={`conn-detail-note-${model.c.name}`}
          intro={<p>What the gateway tells clients about this model of {model.c.name}. The server does not report these itself.</p>}
          onSubmit={async (f) => {
            const n = (v: string | undefined) => (v ? Number(v) : null);
            await api('PUT', `${BASE}/${model.c.id}/models/${encodeURIComponent(model.m.id)}`, {
              kind: f.kind, context_window: Number(f.context_window || 0), max_output_tokens: n(f.max_output_tokens), dimension: n(f.dimension),
              tools: f.tools === 'on', images: f.images === 'on', reasoning: f.reasoning === 'on',
            });
            list.reload();
            return `Saved ${model.m.id}.`;
          }}>
          <div className="two">
            <Field label="Kind">
              <select name="kind" defaultValue={model.m.kind}><option>chat</option><option>embed</option><option>rerank</option><option>image</option></select>
            </Field>
            <Field label="Context window (tokens)"><input name="context_window" type="number" min={0} defaultValue={model.m.context_window} /></Field>
            <Field label="Max output (tokens)"><input name="max_output_tokens" type="number" min={1} defaultValue={model.m.max_output_tokens ?? ''} /></Field>
            <Field label="Embedding dimension"><input name="dimension" type="number" min={1} defaultValue={model.m.dimension ?? ''} /></Field>
          </div>
          <div className="actions">
            <label className="check"><input name="tools" type="checkbox" defaultChecked={model.m.tools} /> Calls tools</label>
            <label className="check"><input name="images" type="checkbox" defaultChecked={model.m.images} /> Reads images</label>
            <label className="check"><input name="reasoning" type="checkbox" defaultChecked={model.m.reasoning} /> Reasons</label>
          </div>
        </Dialog>
      )}
    </>
  );
}
