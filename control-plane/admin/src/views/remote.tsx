// Remote task administration (control-plane/admin-console Remote Task Administration): the dispatch rules of
// control-plane/policy-service, and the per-issuer rate limits and callback allowlist of control-plane/task-dispatch.
import { api } from '../auth.ts';
import { Card, confirmed, Err, Field, Form, Note, time, useAction, useLoad } from '../ui.tsx';

const R = '/api/v1/admin/policy/dispatch-rules';
const T = '/api/v1/admin/tasks';

export function Remote() {
  const rules = useLoad<{ id: number; issuer_kind: string; issuer: string; target_kind: string; target: string }[]>(R);
  const limits = useLoad<{ default_per_hour: number; limits: { issuer: string; per_hour: number; updated_by: string; updated_at: string }[] }>(`${T}/limits`);
  const callbacks = useLoad<{ prefix: string; added_by: string; added_at: string }[]>(`${T}/callbacks`);
  const act = useAction();
  return (
    <div className="grid">
      <Card title="Dispatch rules" id="dispatch-rules">
        <p className="muted">A Remote task is accepted only when a rule matches its issuer and its target.</p>
        <table>
          <thead><tr><th>Issuer</th><th>Target</th><th /></tr></thead>
          <tbody>
            {rules.data?.map((r) => (
              <tr key={r.id} data-testid={`rule-${r.issuer}`}>
                <td>{r.issuer_kind} {r.issuer}</td><td>{r.target_kind === 'any' ? 'all users' : `${r.target_kind} ${r.target}`}</td>
                <td><button className="danger" onClick={() => confirmed('Remove this dispatch rule? Matching Remote tasks are refused from then on.') && void act.run(async () => (await api('DELETE', `${R}/${r.id}`), rules.reload()), 'Rule removed.')}>Remove</button></td>
              </tr>
            ))}
          </tbody>
        </table>
        <Form id="rule-add" onSubmit={(f) => act.run(async () => (await api('POST', R, f), rules.reload()), 'Rule added.')}>
          <Field label="Issuer kind"><select name="issuer_kind">{['user', 'role', 'group', 'group-owner', 'service-principal'].map((k) => <option key={k}>{k}</option>)}</select></Field>
          <Field label="Issuer" grow><input name="issuer" required /></Field>
          <Field label="Target kind"><select name="target_kind">{['user', 'group', 'any'].map((k) => <option key={k}>{k}</option>)}</select></Field>
          <Field label="Target (empty for any)" grow><input name="target" /></Field>
          <button>Add rule</button>
        </Form>
      </Card>
      <Card title="Issuer rate limits" id="issuer-limits">
        <p className="muted">
          Tasks each issuer may request per hour. Issuers without a limit here get {limits.data?.default_per_hour ?? 100}. Issuer ids are
          service:&lt;name&gt; for service principals and user:&lt;user name&gt; for people.
        </p>
        <table>
          <thead><tr><th>Issuer</th><th>Per hour</th><th>Changed</th><th /></tr></thead>
          <tbody>
            {limits.data?.limits.map((l) => (
              <tr key={l.issuer} data-testid={`limit-${l.issuer}`}>
                <td>{l.issuer}</td><td>{l.per_hour}</td><td>{time(l.updated_at)} by {l.updated_by}</td>
                <td><button className="danger" onClick={() => confirmed(`Remove the limit of ${l.issuer}?`) && void act.run(async () => (await api('DELETE', `${T}/limits/${encodeURIComponent(l.issuer)}`), limits.reload()), 'Limit removed.')}>Remove</button></td>
              </tr>
            ))}
          </tbody>
        </table>
        <Form id="limit-set" onSubmit={(f) => act.run(async () => (await api('PUT', `${T}/limits`, { issuer: f.issuer, per_hour: Number(f.per_hour) }), limits.reload()), 'Limit saved. It applies to the next request.')}>
          <Field label="Issuer id" grow><input name="issuer" placeholder="service:ci" required /></Field>
          <Field label="Tasks per hour"><input name="per_hour" type="number" min="0" required /></Field>
          <button>Set limit</button>
        </Form>
      </Card>
      <Card title="Callback allowlist" id="callback-allowlist">
        <p className="muted">A task may name a result callback only under one of these URL prefixes.</p>
        <ul className="entries">
          {callbacks.data?.map((c) => (
            <li key={c.prefix} data-testid={`callback-${c.prefix}`}>
              <span>{c.prefix} <small>({time(c.added_at)} by {c.added_by})</small></span>
              <button className="icon danger" aria-label={`Remove ${c.prefix}`} title="Remove" onClick={() => confirmed(`Remove the prefix ${c.prefix}?`) && void act.run(async () => (await api('POST', `${T}/callbacks/remove`, { prefix: c.prefix }), callbacks.reload()), 'Prefix removed.')}>×</button>
            </li>
          ))}
        </ul>
        <Form id="callback-add" onSubmit={(f) => act.run(async () => (await api('POST', `${T}/callbacks`, { prefix: f.prefix }), callbacks.reload()), 'Prefix added.')}>
          <Field label="URL prefix" grow><input name="prefix" placeholder="https://ci.example.com/hooks/" required /></Field>
          <button>Add prefix</button>
        </Form>
      </Card>
      <Err error={act.error || rules.error || limits.error || callbacks.error} id="remote-error" />
      <Note text={act.note} id="remote-note" />
    </div>
  );
}
