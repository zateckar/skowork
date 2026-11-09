// Audit log viewer and export (Audit Log Viewer), individual-level data access with a stated reason
// (Individual-Level Data Access), and the access records the works council reviews. The log is a filtered table of
// records; each event opens in a dialog with its details (redesign-admin-console-records D3).
import { useState } from 'react';
import { api, download } from '../auth.ts';
import { Card, Dialog, Err, Field, Form, Note, Picker, qs, Records, show, time, useAction, useDialog, useLoad, useUsers } from '../ui.tsx';

const A = '/api/v1/admin';
type Event = Record<string, any>;

export function Audit() {
  const [filter, setFilter] = useState<Record<string, string>>({});
  const [event, openEvent, closeEvent] = useDialog<Event>();
  const users = useUsers();
  const act = useAction();
  // Searching by user is individual-level access: it needs a reason, so nothing is loaded until one is given.
  const path = filter.user && !filter.reason ? null : `${A}/audit?${qs({ limit: 200, ...filter })}`;
  const rows = useLoad<Event[]>(path);
  const exp = (format: string) => void act.run(() => download(`${A}/audit/export?${qs({ format, ...filter })}`), 'Exported.');
  return (
    <>
      <Card title="Find events" id="audit-search" sub="Searching the events of one user is individual-level access: state why, and the access is recorded.">
        <Form id="audit-filter" onSubmit={(f) => setFilter({
          ...f,
          since: f.since ? new Date(f.since).toISOString() : '',
          until: f.until ? new Date(f.until).toISOString() : '',
        })}>
          <Field label="Category"><input name="category" placeholder="policy" /></Field>
          <Field label="Action"><input name="action" placeholder="policy.publish" /></Field>
          <Field label="Outcome"><select name="outcome"><option value="">Any</option><option>ok</option><option>refused</option><option>error</option></select></Field>
          <Field label="Since"><input name="since" type="datetime-local" /></Field>
          <Field label="Until"><input name="until" type="datetime-local" /></Field>
          <Field label="User"><Picker name="user" options={users} allowNew newHint="not seen yet" testid="audit-user" label="User" placeholder="Anyone" /></Field>
          <Field label="Reason for searching a user" grow><input name="reason" /></Field>
          <button className="primary">Search</button>
        </Form>
        {!path && <p className="err" role="alert">State a reason to search the events of a user.</p>}
      </Card>
      <Records
        title="Audit log"
        id="audit"
        items={rows.data}
        error={rows.error}
        text={(r) => `${r.category} ${r.action} ${r.outcome} ${r.user_name ?? r.subject ?? ''} ${r.reason ?? ''}`}
        tools={
          <>
            <button data-testid="export-jsonl" onClick={() => exp('jsonl')}>Export JSON Lines</button>
            <button data-testid="export-csv" onClick={() => exp('csv')}>Export CSV</button>
          </>
        }
        head={<tr><th>Time</th><th>Event</th><th>Outcome</th><th>By or for</th><th>Reason</th><th /></tr>}
        empty="No event matches this search."
        row={(r) => (
          <tr key={r.id} data-testid={`audit-${r.id}`}>
            <td>{time(r.at)}</td>
            <td><b>{r.action}</b><small>{r.category}</small></td>
            <td><span className={`state ${r.outcome === 'ok' ? 'ok' : r.outcome === 'refused' ? 'pending' : 'failing'}`}>{r.outcome}</span></td>
            <td>{r.user_name ?? r.subject}{r.installation_id && <small>{r.installation_id}</small>}</td>
            <td>{r.reason}</td>
            <td className="acts">{r.details && <button className="link" onClick={() => openEvent(r)}>Details</button>}</td>
          </tr>
        )}
      />
      <Err error={act.error} id="audit-error" />
      <Note text={act.note} />
      {event && (
        <Dialog id="audit-event" title={event.action} wide onClose={closeEvent}>
          <div className="facts">
            <div><span>When</span><b>{time(event.at)}</b></div>
            <div><span>Outcome</span><b>{event.outcome}</b></div>
            <div><span>By or for</span><b>{event.user_name ?? event.subject ?? '—'}</b></div>
            {event.installation_id && <div><span>Installation</span><b>{event.installation_id}</b></div>}
          </div>
          {event.reason && <p>Reason: {event.reason}</p>}
          <pre>{JSON.stringify(event.details, null, 2)}</pre>
        </Dialog>
      )}
    </>
  );
}

export function Individual() {
  const [data, setData] = useState<{ user: string; records: any[]; crashes: any[]; reason: string } | null>(null);
  const [who, setWho] = useState<{ pseudonym: string; kind: string; value: string } | null>(null);
  const users = useUsers();
  const act = useAction();
  return (
    <>
      <Card title="Individual-level telemetry" id="individual" sub="Every access is recorded with your reason before any data is shown, and the works council can review it.">
        <Form id="individual-form" onSubmit={(f) => act.run(async () => {
          const d = await api<{ user: string; records: any[]; crashes: any[] }>('GET', `${A}/individual/telemetry?${qs({ user: f.user, reason: f.reason })}`);
          setData({ ...d, reason: f.reason ?? '' });
        })}>
          <Field label="User"><Picker name="user" options={users} allowNew newHint="not seen yet" required testid="individual-user" label="User" /></Field>
          <Field label="Reason" grow><input name="reason" placeholder="Ticket or incident, and why" /></Field>
          <button className="primary">Open</button>
        </Form>
        {data && (
          <>
            <h3>Crash reports</h3>
            <ul className="entries">
              {data.crashes.map((c) => (
                <li key={c.id}>
                  <span>{time(c.received_at)}, {c.process}, {c.size} bytes</span>
                  <button onClick={() => void act.run(() => download(`${A}/individual/crashes/${c.id}?${qs({ reason: data.reason })}`))}>Download dump</button>
                </li>
              ))}
              {!data.crashes.length && <li className="muted">None.</li>}
            </ul>
            <h3>Records <span className="tally">{data.records.length}</span></h3>
            <div className="scroll">
              <table data-testid="individual-records">
                <thead><tr><th>Time</th><th>Event</th><th>Installation</th><th>Fields</th></tr></thead>
                <tbody>
                  {data.records.map((r) => (
                    <tr key={r.id}><td>{time(r.at)}</td><td>{r.event}</td><td>{r.installation_id}</td><td><code className="clip">{show(r.attrs)}</code></td></tr>
                  ))}
                </tbody>
              </table>
            </div>
          </>
        )}
      </Card>
      <Card title="Re-identify a pseudonym" id="reidentify" sub="Telemetry names users and machines by pseudonyms. Re-identifying one is recorded like any individual-level access.">
        <Form id="reidentify-form" onSubmit={(f) => act.run(async () => setWho(await api('GET', `${A}/individual/pseudonyms/${encodeURIComponent(f.pseudonym ?? '')}?${qs({ reason: f.reason })}`)))}>
          <Field label="Pseudonym"><input name="pseudonym" placeholder="u_… or m_…" required /></Field>
          <Field label="Reason" grow><input name="reason" /></Field>
          <button className="primary">Re-identify</button>
        </Form>
        {who && <p className="callout" data-testid="reidentified">{who.pseudonym} is {who.kind} {who.value}</p>}
      </Card>
      <Err error={act.error} id="individual-error" />
    </>
  );
}

export function AccessRecords() {
  const rows = useLoad<{ id: number; at: string; auditor: string; user: string | null; kind: string; reason: string }[]>(`${A}/access-records?limit=500`);
  return (
    <Records
      title="Records of individual-level access"
      id="access-records"
      sub="Every time an auditor opened data about one person, with the reason they gave."
      items={rows.data}
      error={rows.error}
      text={(r) => `${r.auditor} ${r.user ?? ''} ${r.kind} ${r.reason}`}
      head={<tr><th>Time</th><th>Auditor</th><th>User</th><th>Access</th><th>Reason</th></tr>}
      empty="No individual-level data has been accessed."
      row={(r) => (
        <tr key={r.id} data-testid={`access-${r.id}`}><td>{time(r.at)}</td><td>{r.auditor}</td><td>{r.user ?? ''}</td><td>{r.kind}</td><td>{r.reason}</td></tr>
      )}
    />
  );
}
