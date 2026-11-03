// LLM connections (control-plane/admin-console LLM Connection Administration): the OpenAI-compatible upstreams of
// control-plane/llm-gateway, their status, discovered models and per-model capabilities. Credentials are write-only.
import { useState } from 'react';
import { api } from '../auth.ts';
import { Card, Err, Field, Form, Note, confirmed, time, useAction, useLoad } from '../ui.tsx';

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

interface Use {
  scope: string;
  role: string;
  model: string;
}

const BASE = '/api/v1/admin/llm/connections';

export function Connections() {
  const list = useLoad<{ connections: Connection[]; key_available: boolean }>(BASE);
  const act = useAction();
  const [editing, setEditing] = useState<number | null>(null);

  // Resolves to true once the connection is saved, which clears the form (also when its test failed).
  const add = async (f: Record<string, string>) => {
    let saved = false;
    await act.run(async () => {
      const c = await api<Connection>('POST', BASE, { name: f.name, base_url: f.base_url, ...(f.credential ? { credential: f.credential } : {}), max_concurrent: Number(f.max_concurrent || 0) });
      saved = true;
      list.reload();
      if (c.status !== 'ok') throw new Error(`Saved, but the connection is ${c.status}: ${c.status_detail ?? ''}`);
    }, 'Connection saved and its models listed.');
    return saved;
  };

  return (
    <>
      <Card title="LLM connections" id="connections">
        <p className="muted">
          The model servers the gateway forwards to. A role mapping names a model id; it goes to the first enabled connection that offers it, or to one
          connection when written as <code>connection/model</code>. Credentials are stored encrypted and are never shown again.
        </p>
        {list.data && !list.data.key_available && <p className="err" role="alert">WORKBENCH_SECRET_KEY is not set on the server, so credentials cannot be stored.</p>}
        <Err error={list.error} />
        {list.data?.connections.length === 0 && <p className="empty">No connection yet. Add the organization LLM API below.</p>}
        <ul className="entries">
          {list.data?.connections.map((c) => (
            <li key={c.id} data-testid={`conn-${c.name}`}>
              <div>
                <strong>{c.name}</strong> <span className={`badge ${c.status}`} data-testid={`conn-status-${c.name}`}>{c.enabled ? c.status : 'disabled'}</span>
                <div className="muted small">
                  {c.base_url} · credential {c.credential_set ? `…${c.credential_hint || '(set)'}` : 'not set'} · {c.models.length} models
                  {c.max_concurrent ? ` · at most ${c.max_concurrent} at once` : ''}
                  {c.status_detail ? ` · ${c.status_detail}` : ''}
                  {c.checked_at ? ` · checked ${time(c.checked_at)}` : ''}
                </div>
              </div>
              <span className="row">
                <button onClick={() => setEditing(editing === c.id ? null : c.id)} data-testid={`conn-open-${c.name}`}>{editing === c.id ? 'Close' : 'Edit and models'}</button>
                <button onClick={() => void act.run(async () => { await api('POST', `${BASE}/${c.id}/test`); list.reload(); }, `Tested ${c.name}.`)} data-testid={`conn-test-${c.name}`}>Test</button>
                <button
                  onClick={() => {
                    if (c.enabled && !confirmed(`Disable ${c.name}? Requests for its models go to other connections or fail.`)) return;
                    void act.run(async () => { await api('PATCH', `${BASE}/${c.id}`, { enabled: !c.enabled }); list.reload(); }, c.enabled ? `Disabled ${c.name}.` : `Enabled ${c.name}.`);
                  }}
                  data-testid={`conn-toggle-${c.name}`}
                >
                  {c.enabled ? 'Disable' : 'Enable'}
                </button>
                <button className="danger" onClick={() => void remove(c, act.run, list.reload)} data-testid={`conn-delete-${c.name}`}>Delete</button>
              </span>
              {editing === c.id && <ConnectionDetail c={c} onChanged={list.reload} />}
            </li>
          ))}
        </ul>
        <Err error={act.error} id="connections-error" />
        <Note text={act.note} id="connections-note" />
      </Card>
      <Card title="Add a connection" id="connection-add-card">
        <Form id="connection-add" onSubmit={add} clearOnSuccess>
          <Field label="Name"><input name="name" required pattern="[a-z0-9][a-z0-9-]{0,39}" placeholder="primary" /></Field>
          <Field label="Base URL" grow><input name="base_url" required type="url" placeholder="https://llm.example.com/v1" /></Field>
          <Field label="Credential (API key)"><input name="credential" type="password" autoComplete="off" /></Field>
          <Field label="Concurrent requests (0: no limit)"><input name="max_concurrent" type="number" min={0} defaultValue={0} /></Field>
          <button className="primary">Add and test</button>
        </Form>
      </Card>
    </>
  );
}

async function remove(c: Connection, run: (fn: () => Promise<unknown>, note?: string) => Promise<boolean>, reload: () => void) {
  let uses: Use[] = [];
  try {
    uses = (await api<{ uses: Use[] }>('GET', `${BASE}/${c.id}/uses`)).uses;
  } catch { /* the delete below reports it */ }
  const named = uses.map((u) => `${u.scope.replace(/^group:/, 'Group ')}: ${u.role} → ${u.model}`).join('\n');
  const question = uses.length
    ? `${c.name} is used by these role mappings:\n${named}\n\nThose roles stop working unless another connection offers the model. Delete anyway?`
    : `Delete ${c.name}? Its stored credential is removed.`;
  if (!confirmed(question)) return;
  await run(async () => {
    await api('DELETE', `${BASE}/${c.id}${uses.length ? '?confirm=true' : ''}`);
    reload();
  }, `Deleted ${c.name}.`);
}

function ConnectionDetail({ c, onChanged }: { c: Connection; onChanged: () => void }) {
  const act = useAction();
  const save = (f: Record<string, string>) =>
    act.run(async () => {
      await api('PATCH', `${BASE}/${c.id}`, {
        name: f.name, base_url: f.base_url, max_concurrent: Number(f.max_concurrent || 0), position: Number(f.position || 0),
        ...(f.credential ? { credential: f.credential } : {}),
      });
      onChanged();
    }, 'Saved.');
  const setModel = (m: Model, f: Record<string, string>) =>
    act.run(async () => {
      const n = (v: string | undefined) => (v ? Number(v) : null);
      await api('PUT', `${BASE}/${c.id}/models/${encodeURIComponent(m.id)}`, {
        kind: f.kind, context_window: Number(f.context_window || 0), max_output_tokens: n(f.max_output_tokens), dimension: n(f.dimension),
        tools: f.tools === 'on', images: f.images === 'on', reasoning: f.reasoning === 'on',
      });
      onChanged();
    }, `Saved ${m.id}.`);
  return (
    <div className="detail" data-testid={`conn-detail-${c.name}`}>
      <Form id={`conn-edit-${c.name}`} onSubmit={save}>
        <Field label="Name"><input name="name" defaultValue={c.name} required /></Field>
        <Field label="Base URL" grow><input name="base_url" defaultValue={c.base_url} required /></Field>
        <Field label="New credential (empty: keep)"><input name="credential" type="password" autoComplete="off" /></Field>
        <Field label="Concurrent requests"><input name="max_concurrent" type="number" min={0} defaultValue={c.max_concurrent} /></Field>
        <Field label="Order"><input name="position" type="number" min={0} defaultValue={c.position} /></Field>
        <button className="primary">Save</button>
      </Form>
      <h3>Models</h3>
      {c.models.length === 0 && <p className="empty">No models listed yet. Use Test.</p>}
      <div className="models">
        {c.models.map((m) => (
          <ModelRow key={m.id} conn={c.name} m={m} onSave={(f) => setModel(m, f)} />
        ))}
      </div>
      <Err error={act.error} id={`conn-detail-error-${c.name}`} />
      <Note text={act.note} id={`conn-detail-note-${c.name}`} />
    </div>
  );
}

function ModelRow({ conn, m, onSave }: { conn: string; m: Model; onSave: (f: Record<string, string>) => void }) {
  const id = `model-${conn}-${m.id}`;
  return (
    <div className="model-row" data-testid={id}>
      <strong className="model-id">
        {m.id}
        {m.missing && <span className="badge failing" title="No longer listed by the connection">missing</span>}
      </strong>
      <Form id={`${id}-form`} onSubmit={onSave}>
        <Field label="Kind">
          <select name="kind" defaultValue={m.kind}><option>chat</option><option>embed</option><option>rerank</option><option>image</option></select>
        </Field>
        <Field label="Context window"><input name="context_window" type="number" min={0} defaultValue={m.context_window} /></Field>
        <Field label="Max output"><input name="max_output_tokens" type="number" min={1} defaultValue={m.max_output_tokens ?? ''} /></Field>
        <Field label="Dimension"><input name="dimension" type="number" min={1} defaultValue={m.dimension ?? ''} /></Field>
        <label className="check"><input name="tools" type="checkbox" defaultChecked={m.tools} /> tools</label>
        <label className="check"><input name="images" type="checkbox" defaultChecked={m.images} /> images</label>
        <label className="check"><input name="reasoning" type="checkbox" defaultChecked={m.reasoning} /> reasoning</label>
        <button aria-label={`Save ${m.id}`}>Save</button>
      </Form>
    </div>
  );
}