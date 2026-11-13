// LLM connections (control-plane/admin-console LLM Connection Administration): the OpenAI-compatible upstreams of
// control-plane/llm-gateway as records with their status; each opens to its discovered models. Adding and editing a
// connection, and the capabilities, reasoning effort and fallback of a model, happen in dialogs. Credentials are
// write-only (redesign-admin-console-records D3). A refused effort or fallback is shown next to its field
// (control-plane/admin-console Model Fallback And Effort Administration).
import { Fragment, useState } from 'react';
import { ApiError, api } from '../auth.ts';
import { ask, Dialog, Field, notify, Records, time, useDialog, useLoad } from '../ui.tsx';

/** The reasoning efforts of control-plane/llm-gateway Reasoning Effort Setting. */
const EFFORTS = ['none', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max'] as const;
/** The fallback wait of a model without a stored one. */
const FALLBACK_AFTER_DEFAULT_S = 60;

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
  reasoning_effort: string | null;
  fallback_model: string | null;
  fallback_after_s: number | null;
  /** The models whose fallback this model is, as `connection/id`. */
  fallback_of: string[];
}

interface Connection {
  id: number;
  name: string;
  base_url: string;
  enabled: boolean;
  credential_set: boolean;
  credential_hint: string;
  max_concurrent: number;
  position: number;
  status: string;
  status_detail: string | null;
  checked_at: string | null;
  models: Model[];
}

interface EffortFailure { model: string; reasoning_effort: string; reason: string }
interface Use { scope: string; role: string; model: string }
/** A refused value of the model editor: the field it belongs to and the gateway's reason. */
interface Refusal { field: string; message: string; nonce: number }

const BASE = '/api/v1/admin/llm/connections';
const caps = (m: Model) => [m.tools && 'tools', m.images && 'images', m.reasoning && 'reasoning'].filter(Boolean).join(', ') || '—';

/** The chat models a fallback may name: listed by an enabled connection, and without a fallback of their own. */
function fallbackChoices(connections: Connection[], self: Connection, model: Model) {
  return connections
    .filter((c) => c.enabled)
    .flatMap((c) => c.models.filter((m) => m.kind === 'chat' && !m.missing && m.fallback_model === null).map((m) => ({ value: `${c.name}/${m.id}`, id: m.id, connection: c.name })))
    .filter((o) => !(o.connection === self.name && o.id === model.id));
}

export function Connections() {
  const list = useLoad<{ connections: Connection[]; key_available: boolean }>(BASE);
  const [open, setOpen] = useState<number | null>(null);
  const [edit, openEdit, closeEdit] = useDialog<Connection | 'new'>();
  const [model, openModel, closeModel] = useDialog<{ c: Connection; m: Model }>();
  const [refusal, setRefusal] = useState<Refusal | null>(null);

  const test = async (c: Connection) => {
    const r = await api<Connection & { effort_failures?: EffortFailure[] }>('POST', `${BASE}/${c.id}/test`);
    list.reload();
    const base = r.status === 'ok' ? `${c.name} answers and lists ${r.models.length} models.` : `Tested ${c.name}: ${r.status}${r.status_detail ? `, ${r.status_detail}` : ''}.`;
    const stale = r.effort_failures ?? [];
    notify(stale.length ? `${base} ${stale.map((f) => `${f.model} refuses reasoning effort ${f.reasoning_effort} (${f.reason})`).join('; ')}.` : base, 'connections-note');
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
  const openModelEditor = (x: { c: Connection; m: Model }) => {
    setRefusal(null);
    openModel(x);
  };
  const closeModelEditor = () => {
    setRefusal(null);
    closeModel();
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
        tools={list.data && !list.data.key_available && <span className="state failing" role="alert">SKOWORK_SECRET_KEY is not set: credentials cannot be stored</span>}
        head={<tr><th>Connection</th><th>Status</th><th>Credential</th><th>Models</th><th>Checked</th><th /></tr>}
        add={<button className="primary" data-testid="connection-add-open" onClick={() => openEdit('new')}>Add connection</button>}
        empty="No connection yet. Add the organization LLM API to give the gateway models."
        row={(c) => (
          <Fragment key={c.id}>
            <tr data-testid={`conn-${c.name}`}>
              <td><b>{c.name}</b><small>{c.base_url}</small></td>
              <td>
                <span className={`state ${c.enabled ? c.status : ''}`} data-testid={`conn-status-${c.name}`}>{c.enabled ? c.status : 'disabled'}</span>
                {c.status_detail && <small className={c.status === 'ok' ? '' : 'err'}>{c.status_detail}</small>}
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
                      <thead><tr><th>Model</th><th>Kind</th><th>Context window</th><th>Max output</th><th>Capabilities</th><th>Reasoning effort</th><th>Fallback</th><th /></tr></thead>
                      <tbody>
                        {c.models.map((m) => (
                          <tr key={m.id} data-testid={`model-${c.name}-${m.id}`}>
                            <td><code>{m.id}</code>{m.missing && <small><span className="state failing" title="No longer listed by the connection">missing</span></small>}</td>
                            <td>{m.kind}{m.dimension ? <small>{m.dimension} dimensions</small> : null}</td>
                            <td>{m.context_window || '—'}</td><td>{m.max_output_tokens ?? '—'}</td><td>{caps(m)}</td>
                            <td data-testid={`model-${c.name}-${m.id}-effort`}>{m.kind === 'chat' ? (m.reasoning_effort ?? <span className="muted">model default</span>) : '—'}</td>
                            <td data-testid={`model-${c.name}-${m.id}-fallback`}>
                              {m.kind === 'chat' && m.fallback_model ? <><code>{m.fallback_model}</code><small>after {m.fallback_after_s ?? FALLBACK_AFTER_DEFAULT_S} s</small></> : <span className="muted">none</span>}
                              {m.fallback_of.length > 0 && <small>fallback of {m.fallback_of.join(', ')}</small>}
                            </td>
                            <td className="acts"><button className="link" data-testid={`model-${c.name}-${m.id}-edit`} onClick={() => openModelEditor({ c, m })}>Edit</button></td>
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

      {model && (() => {
        const { c, m } = model;
        const refused = (field: string) => (refusal?.field === field ? <span className="err" role="alert" data-testid={`model-refused-${field}`}>{refusal.message}</span> : null);
        const choices = fallbackChoices(list.data?.connections ?? [], c, m);
        const stored = m.fallback_model;
        const storedChoice = stored ? choices.find((o) => o.value === stored) ?? choices.find((o) => o.id === stored) : undefined;
        const selected = storedChoice?.value ?? stored ?? '';
        // A model that another model falls back to cannot have a fallback itself (control-plane/llm-gateway Fallback Model Setting).
        const blocked = m.fallback_of.length > 0;
        return (
          <Dialog id={`model-${c.name}-${m.id}-form`} title={`Settings of ${m.id}`} submit="Save" onClose={closeModelEditor} noteId={`conn-detail-note-${c.name}`}
            intro={<p>What the gateway tells clients about this model of {c.name}, and how it reasons and falls back. The server does not report these itself.</p>}
            onSubmit={async (f) => {
              try {
                await api('PUT', `${BASE}/${c.id}/models/${encodeURIComponent(m.id)}`, {
                  kind: f.kind, context_window: Number(f.context_window || 0), max_output_tokens: f.max_output_tokens ? Number(f.max_output_tokens) : null,
                  dimension: f.dimension ? Number(f.dimension) : null,
                  tools: f.tools === 'on', images: f.images === 'on', reasoning: f.reasoning === 'on',
                  reasoning_effort: f.reasoning_effort || null,
                  // Not sent while the picker is blocked: the gateway keeps the stored value, which is empty for such a model.
                  ...(f.fallback_model === undefined ? {} : { fallback_model: f.fallback_model || null }),
                  fallback_after_s: f.fallback_after_s ? Number(f.fallback_after_s) : null,
                });
              } catch (err) {
                if (err instanceof ApiError && typeof err.details.field === 'string') {
                  setRefusal({ field: err.details.field, message: err.message, nonce: (refusal?.nonce ?? 0) + 1 });
                  throw new Error('Not saved. The value marked above was refused, and the stored value is unchanged.');
                }
                throw err;
              }
              list.reload();
              return `Saved ${m.id}.`;
            }}>
            <div className="two">
              <Field label="Kind">
                <select name="kind" defaultValue={m.kind}><option>chat</option><option>embed</option><option>rerank</option><option>image</option></select>
              </Field>
              <Field label="Context window (tokens)"><input name="context_window" type="number" min={0} defaultValue={m.context_window} /></Field>
              <Field label="Max output (tokens)"><input name="max_output_tokens" type="number" min={1} defaultValue={m.max_output_tokens ?? ''} /></Field>
              <Field label="Embedding dimension"><input name="dimension" type="number" min={1} defaultValue={m.dimension ?? ''} /></Field>
            </div>
            <div className="actions">
              <label className="check"><input name="tools" type="checkbox" defaultChecked={m.tools} /> Calls tools</label>
              <label className="check"><input name="images" type="checkbox" defaultChecked={m.images} /> Reads images</label>
              <label className="check"><input name="reasoning" type="checkbox" defaultChecked={m.reasoning} /> Reasons</label>
            </div>
            <div className="two">
              <Field key={`effort-${refusal?.nonce ?? 0}`} label="Reasoning effort"
                help={refused('reasoning_effort') ?? 'Model default sends nothing. A value is checked with the model when saved.'}>
                <select name="reasoning_effort" defaultValue={m.reasoning_effort ?? ''}>
                  <option value="">Model default</option>
                  {EFFORTS.map((e) => <option key={e} value={e}>{e}</option>)}
                </select>
              </Field>
              <Field key={`fallback-${refusal?.nonce ?? 0}`} label="Fallback model"
                help={refused('fallback_model') ?? (blocked ? `${m.id} is the fallback of ${m.fallback_of.join(', ')}, so it cannot have one.` : 'Used when this model does not answer before the wait below. Chat models only.')}>
                <select name={blocked ? undefined : 'fallback_model'} disabled={blocked} defaultValue={selected}>
                  <option value="">No fallback</option>
                  {choices.map((o) => <option key={o.value} value={o.value}>{o.id} ({o.connection})</option>)}
                  {stored && !storedChoice && <option value={stored}>{stored} (not offered now)</option>}
                </select>
              </Field>
            </div>
            <div className="two">
              <Field key={`wait-${refusal?.nonce ?? 0}`} label="Fallback wait (seconds)"
                help={refused('fallback_after_s') ?? '5 to 600. Empty means 60 seconds.'}>
                <input name="fallback_after_s" type="number" min={5} max={600} placeholder={String(FALLBACK_AFTER_DEFAULT_S)} defaultValue={m.fallback_after_s ?? ''} />
              </Field>
            </div>
          </Dialog>
        );
      })()}
    </>
  );
}
