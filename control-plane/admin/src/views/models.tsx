// Model and quota administration (control-plane/admin-console Model And Quota Administration): per Group, the model
// role mappings (policy.llm.models, also the model allowlist), token budgets and rate limits. The keys belong to
// control-plane/llm-gateway; the view edits them in the Group's policy draft and publishes through the policy service.
import { useEffect, useState } from 'react';
import { api, ApiError } from '../auth.ts';
import { Card, Err, Field, Form, Note, useAction, useLoad } from '../ui.tsx';
import { FieldErrors, scopePath, type FieldError } from './policy.tsx';

const ROLES = ['main', 'vision', 'small', 'embed', 'rerank', 'image'] as const;
const KIND_OF: Record<string, string> = { main: 'chat', vision: 'chat', small: 'chat', embed: 'embed', rerank: 'rerank', image: 'image' };

interface CatalogModel {
  connection: string;
  id: string;
  kind: string;
  enabled: boolean;
  missing: boolean;
}

/**
 * The model of a role, chosen from the discovered models of the LLM connections. The first connection offering an
 * id gets the bare id (failover to the others); later ones are pinned as connection/id. A model of the wrong kind
 * for the role is marked.
 */
function ModelPicker({ role, value, inherited, catalog }: { role: string; value: string; inherited?: string; catalog: CatalogModel[] }) {
  const first = new Map<string, string>();
  for (const m of catalog) if (m.enabled && !first.has(m.id)) first.set(m.id, m.connection);
  const options = catalog.map((m) => {
    const v = first.get(m.id) === m.connection ? m.id : `${m.connection}/${m.id}`;
    const notes = [m.kind !== KIND_OF[role] ? `${m.kind} model, not ${KIND_OF[role]}` : '', m.enabled ? '' : 'connection disabled', m.missing ? 'no longer listed' : '']
      .filter(Boolean);
    return { v, label: `${m.id} · ${m.connection} · ${m.kind}${notes.length ? ` ⚠ ${notes.join(', ')}` : ''}` };
  });
  const known = !value || options.some((o) => o.v === value);
  return (
    <select name={`model.${role}`} defaultValue={value} data-testid={`model-${role}`}>
      <option value="">{inherited ? `(inherited: ${inherited})` : '(inherited)'}</option>
      {!known && <option value={value}>{value} ⚠ not offered by any connection</option>}
      {options.map((o) => <option key={o.v} value={o.v}>{o.label}</option>)}
    </select>
  );
}
const NUMBERS = [
  ['budget.tokens', 'Token budget per user'],
  ['budget.embed_daily_tokens', 'Embedding tokens per day'],
  ['budget.images_daily', 'Images per day'],
  ['rate_limits.requests_per_minute', 'Requests per minute'],
  ['rate_limits.concurrent_requests', 'Concurrent requests'],
] as const;

type Doc = Record<string, any>;

export function Models() {
  const groups = useLoad<{ name: string }[]>('/api/v1/admin/policy/groups');
  const catalog = useLoad<{ models: CatalogModel[] }>('/api/v1/admin/llm/models');
  const schema = useLoad<{ schema: Doc }>('/api/v1/admin/policy/schema');
  const [group, setGroup] = useState('');
  const [base, setBase] = useState<Record<string, string>>({});
  const [doc, setDoc] = useState<Doc | null>(null);
  const [priority, setPriority] = useState(0);
  const [errors, setErrors] = useState<{ doc: unknown; errors: FieldError[] } | null>(null);
  const act = useAction();

  useEffect(() => {
    if (!group) return setDoc(null);
    (async () => {
      for (const ref of ['draft', 'current']) {
        try {
          const v = await api<{ document: Doc; priority: number }>('GET', `${scopePath(group)}/versions/${ref}`);
          setDoc(v.document);
          setPriority(v.priority);
          return;
        } catch { /* next */ }
      }
      setDoc({});
    })();
  }, [group]);

  // What a role resolves to when this Group leaves it out: the everyone Group's mapping, else the gateway defaults.
  // policy.llm.models merges as a whole map, so a Group that maps any role must list every role it keeps.
  useEffect(() => {
    const defaults = (schema.data?.schema?.properties?.llm?.properties?.models?.default ?? {}) as Record<string, string>;
    if (!group || group === 'everyone') return setBase(defaults);
    api<{ document: Doc }>('GET', `${scopePath('everyone')}/versions/current`)
      .then((v) => setBase((v.document?.llm?.models as Record<string, string> | undefined) ?? defaults))
      .catch(() => setBase(defaults));
  }, [group, schema.data]);

  const llm = (doc?.llm ?? {}) as Doc;
  const get = (path: string) => path.split('.').reduce<any>((o, k) => o?.[k], llm);

  const save = (f: Record<string, string>) => act.run(async () => {
    const next: Doc = structuredClone(doc ?? {});
    const l: Doc = (next.llm ??= {});
    const models: Record<string, string> = {};
    for (const r of ROLES) if (f[`model.${r}`]) models[r] = f[`model.${r}`]!;
    if (Object.keys(models).length) {
      // Roles left as inherited keep their inherited model instead of becoming unavailable (whole-map merge).
      for (const r of ROLES) if (!models[r] && base[r]) models[r] = base[r];
      l.models = models;
    }
    else delete l.models;
    for (const [path] of NUMBERS) {
      const [a, b] = path.split('.') as [string, string];
      if (f[path]) (l[a] ??= {})[b] = Number(f[path]);
      else if (l[a]) delete l[a][b];
    }
    if (f.period) (l.budget ??= {}).period = f.period;
    else if (l.budget) delete l.budget.period;
    for (const k of ['budget', 'rate_limits']) if (l[k] && !Object.keys(l[k]).length) delete l[k];
    if (!Object.keys(l).length) delete next.llm;
    try {
      await api('PUT', `${scopePath(group)}/draft`, { document: next, priority, reason: f.reason });
      setErrors(null);
    } catch (e) {
      if (e instanceof ApiError && Array.isArray(e.details.errors)) setErrors({ doc: next, errors: e.details.errors as FieldError[] });
      throw e;
    }
    setDoc(next);
    if (f.publish === 'on') await api('POST', `${scopePath(group)}/publish`, { reason: f.reason });
  }, f.publish === 'on' ? 'Saved and published. New Runs of the Group use these settings.' : 'Saved as a draft.');

  return (
    <Card title="Models and quotas" id="models">
      <p className="muted">Pick a Group to set its model per role, token budgets and rate limits. Unset values are inherited from the org guardrail and the gateway defaults.</p>
      <label className="row">Group <select data-testid="models-group" value={group} onChange={(e) => setGroup(e.target.value)}>
        <option value="">choose…</option>
        {groups.data?.map((g) => <option key={g.name}>{g.name}</option>)}
      </select></label>
      <p className="muted">When a Group maps any role, the roles left as inherited are saved with the model shown, so they stay available.</p>
      <Err error={groups.error || catalog.error} />
      {catalog.data?.models.length === 0 && <p className="empty">No LLM connection lists any model yet. Add one under <a href="#/connections">LLM connections</a>.</p>}
      {doc && catalog.data && (
        <Form key={group} id="models-form" onSubmit={save}>
          <h3>Models per role</h3>
          <div className="row">
            {ROLES.map((r) => (
              <Field key={r} label={`Model for ${r}`}><ModelPicker role={r} value={get(`models.${r}`) ?? ''} inherited={base[r]} catalog={catalog.data?.models ?? []} /></Field>
            ))}
          </div>
          <h3>Budgets and rate limits</h3>
          <div className="row">
            {NUMBERS.map(([path, label]) => (
              <Field key={path} label={label}><input name={path} type="number" min={0} defaultValue={get(path) ?? ''} placeholder="(inherited)" /></Field>
            ))}
            <Field label="Budget period">
              <select name="period" defaultValue={get('budget.period') ?? ''}><option value="">(inherited)</option><option>day</option><option>week</option><option>month</option></select>
            </Field>
          </div>
          <div className="row">
            <Field label="Reason" grow><input name="reason" required /></Field>
            <label className="check"><input type="checkbox" name="publish" /> publish now</label>
            <button className="primary">Save</button>
          </div>
        </Form>
      )}
      {errors && <FieldErrors doc={errors.doc} errors={errors.errors} />}
      <Err error={act.error} id="models-error" />
      <Note text={act.note} id="models-note" />
    </Card>
  );
}
