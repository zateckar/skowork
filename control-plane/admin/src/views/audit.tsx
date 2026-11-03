// Audit log viewer and export (Audit Log Viewer), individual-level data access with a stated reason
// (Individual-Level Data Access), and the access records the works council reviews.
import { useState } from 'react';
import { api, download } from '../auth.ts';
import { Card, Err, Field, Form, Note, qs, show, time, useAction, useLoad } from '../ui.tsx';

const A = '/api/v1/admin';

export function Audit() {
  const [filter, setFilter] = useState<Record<string, string>>({});
  const act = useAction();
  // Searching by user is individual-level access: it needs a reason, so nothing is loaded until one is given.
  const path = filter.user && !filter.reason ? null : `${A}/audit?${qs({ limit: 200, ...filter })}`;
  const rows = useLoad<Record<string, any>[]>(path);
  return (
    <Card title="Audit log" id="audit">
      <Form id="audit-filter" onSubmit={(f) => setFilter({
        ...f,
        since: f.since ? new Date(f.since).toISOString() : '',
        until: f.until ? new Date(f.until).toISOString() : '',
      })}>
        <Field label="Category"><input name="category" /></Field>
        <Field label="Action"><input name="action" /></Field>
        <Field label="Outcome"><select name="outcome"><option value="">any</option><option>ok</option><option>refused</option><option>error</option></select></Field>
        <Field label="Since"><input name="since" type="datetime-local" /></Field>
        <Field label="Until"><input name="until" type="datetime-local" /></Field>
        <Field label="User (needs a reason)"><input name="user" /></Field>
        <Field label="Reason for searching a user" grow><input name="reason" /></Field>
        <button>Search</button>
      </Form>
      <div className="row">
        <button data-testid="export-jsonl" onClick={() => void act.run(() => download(`${A}/audit/export?${qs({ format: 'jsonl', ...filter })}`), 'Exported.')}>Export JSON Lines</button>
        <button data-testid="export-csv" onClick={() => void act.run(() => download(`${A}/audit/export?${qs({ format: 'csv', ...filter })}`), 'Exported.')}>Export CSV</button>
      </div>
      {!path && <p className="err">State a reason to search the events of a user.</p>}
      <Err error={rows.error || act.error} id="audit-error" />
      <Note text={act.note} />
      <table>
        <thead><tr><th>Time</th><th>Category</th><th>Action</th><th>Outcome</th><th>By / user</th><th>Installation</th><th>Reason</th><th>Details</th></tr></thead>
        <tbody>
          {rows.data?.map((r) => (
            <tr key={r.id} data-testid={`audit-${r.id}`}>
              <td>{time(r.at)}</td><td>{r.category}</td><td>{r.action}</td><td>{r.outcome}</td><td>{r.user_name ?? r.subject}</td>
              <td>{r.installation_id}</td><td>{r.reason}</td><td><code className="clip">{r.details ? show(r.details) : ''}</code></td>
            </tr>
          ))}
        </tbody>
      </table>
    </Card>
  );
}

export function Individual() {
  const [data, setData] = useState<{ user: string; records: any[]; crashes: any[]; reason: string } | null>(null);
  const [who, setWho] = useState<{ pseudonym: string; kind: string; value: string } | null>(null);
  const act = useAction();
  return (
    <>
      <Card title="Individual-level telemetry" id="individual">
        <p className="muted">Every access is recorded with your reason before any data is shown, and the works council can review it.</p>
        <Form id="individual-form" onSubmit={(f) => act.run(async () => {
          const d = await api<{ user: string; records: any[]; crashes: any[] }>('GET', `${A}/individual/telemetry?${qs({ user: f.user, reason: f.reason })}`);
          setData({ ...d, reason: f.reason ?? '' });
        })}>
          <Field label="User"><input name="user" required /></Field> <Field label="Reason (required)" grow><input name="reason" /></Field> <button>Open</button>
        </Form>
        {data && (
          <>
            <h3>Crash reports</h3>
            <ul>
              {data.crashes.map((c) => (
                <li key={c.id}>
                  {time(c.received_at)} · {c.process} · {c.size} bytes{' '}
                  <button onClick={() => void act.run(() => download(`${A}/individual/crashes/${c.id}?${qs({ reason: data.reason })}`))}>Download dump</button>
                </li>
              ))}
              {!data.crashes.length && <li>None.</li>}
            </ul>
            <h3>Records ({data.records.length})</h3>
            <table data-testid="individual-records">
              <thead><tr><th>Time</th><th>Event</th><th>Installation</th><th>Fields</th></tr></thead>
              <tbody>
                {data.records.map((r) => (
                  <tr key={r.id}><td>{time(r.at)}</td><td>{r.event}</td><td>{r.installation_id}</td><td><code>{show(r.attrs)}</code></td></tr>
                ))}
              </tbody>
            </table>
          </>
        )}
      </Card>
      <Card title="Re-identify a pseudonym" id="reidentify">
        <Form id="reidentify-form" onSubmit={(f) => act.run(async () => setWho(await api('GET', `${A}/individual/pseudonyms/${encodeURIComponent(f.pseudonym ?? '')}?${qs({ reason: f.reason })}`)))}>
          <Field label="Pseudonym"><input name="pseudonym" placeholder="u_… or m_…" required /></Field> <Field label="Reason (required)" grow><input name="reason" /></Field> <button>Re-identify</button>
        </Form>
        {who && <p data-testid="reidentified">{who.pseudonym} is {who.kind} {who.value}</p>}
      </Card>
      <Err error={act.error} id="individual-error" />
    </>
  );
}

export function AccessRecords() {
  const rows = useLoad<{ id: number; at: string; auditor: string; user: string | null; kind: string; reason: string }[]>(`${A}/access-records?limit=500`);
  return (
    <Card title="Records of individual-level access" id="access-records">
      <Err error={rows.error} />
      <table>
        <thead><tr><th>Time</th><th>Auditor</th><th>User</th><th>Access</th><th>Reason</th></tr></thead>
        <tbody>
          {rows.data?.map((r) => (
            <tr key={r.id} data-testid={`access-${r.id}`}><td>{time(r.at)}</td><td>{r.auditor}</td><td>{r.user ?? ''}</td><td>{r.kind}</td><td>{r.reason}</td></tr>
          ))}
          {rows.data?.length === 0 && <tr><td colSpan={5}>No individual-level data has been accessed.</td></tr>}
        </tbody>
      </table>
    </Card>
  );
}
