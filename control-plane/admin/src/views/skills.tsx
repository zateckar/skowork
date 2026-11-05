// Skill review queue (control-plane/admin-console Skill Review Queue) over control-plane/skill-registry: the submitted
// org versions with their scan results and the diff against the previous version, approve or reject with a comment,
// public import for platform-admins, and revocation.
import { useRef, useState } from 'react';
import type { Me } from '../App.tsx';
import { api, download, upload } from '../auth.ts';
import { Card, confirmed, Err, Field, Form, Note, qs, time, useAction, useLoad } from '../ui.tsx';

const S = '/api/v1/admin/skills';

interface Version {
  id: number; name: string; version: string; scope: string; audience: string[]; state: string; author: string; description: string;
  risk: string; network: string; tools: string[]; requires_mcp: string[]; findings: number; source_url: string | null; licence: string | null;
  review_comment: string | null; reviewed_by: string | null; created_at: string; revoke_reason: string | null;
}
interface Finding { kind: string; severity: string; file: string; line: number; rule: string; message: string }
interface Detail extends Version {
  scan: { findings: Finding[]; commands: { command: string; file: string; line: number }[]; urls: { url: string; file: string; line: number }[] };
  body: string;
  files: { path: string; size: number }[];
  diff: { against: string; files: { path: string; status: string; lines?: string[]; note?: string }[] } | null;
}

export function Skills({ me }: { me: Me }) {
  const queue = useLoad<Version[]>(`${S}/queue`);
  const all = useLoad<Version[]>(S);
  const [open, setOpen] = useState<number | null>(null);
  const act = useAction();
  const reload = () => (queue.reload(), all.reload());
  const run = (fn: () => Promise<unknown>, note: string) => act.run(async () => (await fn(), reload()), note);
  const admin = me.admin_roles.includes('platform-admin');
  return (
    <>
      <Card title="Skill review queue" id="skill-queue" actions={<button data-testid="skill-refresh" onClick={reload}>Refresh</button>}>
        {queue.data?.length === 0 && <p data-testid="skill-queue-empty">No Skill versions are waiting for review.</p>}
        {!!queue.data?.length && (
          <table>
            <thead><tr><th>Skill</th><th>Author</th><th>Declared</th><th>Findings</th><th>Submitted</th><th /></tr></thead>
            <tbody>
              {queue.data.map((v) => (
                <tr key={v.id} data-testid={`skill-${v.name}-${v.version}`}>
                  <td><b>{v.name}</b> {v.version}<br /><small>{v.description}</small>{v.licence && <><br /><small>import: {v.source_url ?? 'uploaded package'} ({v.licence})</small></>}</td>
                  <td>{v.author}</td>
                  <td><small>risk {v.risk} · tools {v.tools.join(', ') || 'default'}{v.requires_mcp.length ? ` · MCP ${v.requires_mcp.join(', ')}` : ''}</small></td>
                  <td>{v.findings}</td>
                  <td><small>{time(v.created_at)}</small></td>
                  <td><button data-testid={`skill-open-${v.id}`} onClick={() => setOpen(v.id)}>Review</button></td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Card>
      {open !== null && <Review id={open} onDecided={(note) => (setOpen(null), reload(), act.run(async () => {}, note))} />}
      {admin && <Import onDone={(note) => run(async () => {}, note)} />}
      <Card title="Published Skills" id="skill-all">
        <table>
          <thead><tr><th>Skill</th><th>Scope</th><th>State</th><th>Author</th><th>Revoke</th></tr></thead>
          <tbody>
            {all.data?.map((v) => (
              <tr key={v.id} data-testid={`skill-row-${v.name}-${v.version}`}>
                <td><b>{v.name}</b> {v.version}</td>
                <td>{v.scope}{v.audience.length ? `: ${v.audience.join(', ')}` : ''}</td>
                <td data-testid={`skill-state-${v.name}-${v.version}`}>{v.state}{v.revoke_reason ? ` (${v.revoke_reason})` : ''}{v.review_comment ? ` — ${v.review_comment}` : ''}</td>
                <td>{v.author}</td>
                <td>
                  {v.state === 'published' && (
                    <Form id={`skill-revoke-${v.name}-${v.version}`} onSubmit={(f) => confirmed(`Revoke ${v.name} ${v.version}? Clients remove it at their next sync.`) && run(() => api('POST', `${S}/${encodeURIComponent(v.name)}/revoke`, { version: v.version, reason: f.reason }), `${v.name} ${v.version} revoked.`)}>
                      <input name="reason" aria-label={`Reason for revoking ${v.name} ${v.version}`} placeholder="reason" required /> <button className="danger">Revoke</button>
                    </Form>
                  )}
                </td>
              </tr>
            ))}
            {all.data?.length === 0 && <tr><td colSpan={5} className="empty">No Skills have been published yet.</td></tr>}
          </tbody>
        </table>
      </Card>
      <Err error={act.error || queue.error || all.error} id="skill-error" />
      <Note text={act.note} id="skill-note" />
    </>
  );
}

function Review({ id, onDecided }: { id: number; onDecided: (note: string) => void }) {
  const d = useLoad<Detail>(`${S}/versions/${id}`);
  const act = useAction();
  const [comment, setComment] = useState('');
  const v = d.data;
  if (!v) return <Err error={d.error} id="skill-detail-error" />;
  const decide = (what: 'approve' | 'reject') =>
    act.run(async () => {
      await api('POST', `${S}/versions/${id}/${what}`, { comment });
      onDecided(`${v.name} ${v.version} ${what === 'approve' ? 'approved and signed' : 'rejected'}.`);
    });
  return (
    <Card title={`Review ${v.name} ${v.version}`} id="skill-detail" actions={<button onClick={() => void act.run(() => download(`${S}/versions/${id}/content`))}>Download package</button>}>
      <p>{v.description}</p>
      <p data-testid="skill-declared">
        Declared risk <b>{v.risk}</b>, network {v.network}, tools {v.tools.join(', ') || 'default (read-class)'}
        {v.requires_mcp.length ? `, requires MCP ${v.requires_mcp.join(', ')}` : ''}
      </p>
      {v.licence && (
        <p data-testid="skill-source">
          {v.source_url ? <>Imported from <a href={v.source_url} rel="noreferrer noopener" target="_blank">{v.source_url}</a></> : 'Imported as an uploaded package'} under {v.licence}
        </p>
      )}
      <h3>Scan findings</h3>
      <ul data-testid="skill-findings">
        {v.scan.findings.length === 0 && <li>None.</li>}
        {v.scan.findings.map((f, i) => <li key={i}><b>{f.severity}</b> {f.kind} ({f.rule}) — {f.file}:{f.line}: {f.message}</li>)}
      </ul>
      <h3>Script commands</h3>
      <p data-testid="skill-commands">{v.scan.commands.map((c) => c.command).join(', ') || 'None.'}</p>
      <h3>Network endpoints</h3>
      <p data-testid="skill-urls">{v.scan.urls.map((u) => u.url).join(', ') || 'None.'}</p>
      <h3>{v.diff ? `Changes against ${v.diff.against}` : 'Files (first version)'}</h3>
      <div data-testid="skill-diff">
        {v.diff
          ? v.diff.files.filter((f) => f.status !== 'unchanged').map((f) => (
              <details key={f.path} open>
                <summary>{f.path} — {f.status}{f.note ? ` (${f.note})` : ''}</summary>
                {f.lines && <pre className="diff">{f.lines.join('\n')}</pre>}
              </details>
            ))
          : <ul>{v.files.map((f) => <li key={f.path}>{f.path} <small>{f.size} B</small></li>)}</ul>}
      </div>
      <details><summary>Instructions (SKILL.md body)</summary><pre>{v.body}</pre></details>
      <div className="row">
        <Field label="Comment (required to reject)" grow><textarea data-testid="skill-comment" value={comment} onChange={(e) => setComment(e.target.value)} rows={2} /></Field>
        <button data-testid="skill-approve" onClick={() => void decide('approve')}>Approve and sign</button>
        <button data-testid="skill-reject" onClick={() => void decide('reject')}>Reject</button>
      </div>
      <Err error={act.error} id="skill-review-error" />
    </Card>
  );
}

function Import({ onDone }: { onDone: (note: string) => void }) {
  const file = useRef<HTMLInputElement>(null);
  const [source, setSource] = useState<'url' | 'file'>('url');
  const act = useAction();
  return (
    <Card title="Import a public Skill" id="skill-import">
      <Form
        id="skill-import-form"
        onSubmit={(f) =>
          act.run(async () => {
            let v: Version;
            if (source === 'url') {
              v = await api<Version>('POST', `${S}/import?${qs({ url: f.url, licence: f.licence })}`);
            } else {
              const pkg = file.current?.files?.[0];
              if (!pkg) throw new Error('Choose the Skill package (zip).');
              v = await upload<Version>(`${S}/import?${qs({ source_url: f.source_url, licence: f.licence })}`, pkg);
            }
            onDone(`${v.name} ${v.version} entered the review queue.`);
          })
        }
      >
        <Field label="Source">
          <select data-testid="skill-import-source" value={source} onChange={(e) => setSource(e.target.value as 'url' | 'file')}>
            <option value="url">Download from a URL</option>
            <option value="file">Upload a zip file</option>
          </select>
        </Field>
        {source === 'url' ? (
          <Field label="Package URL (zip)" grow><input name="url" type="url" placeholder="https://…/skill.zip" required /></Field>
        ) : (
          <>
            <Field label="Skill package (zip)"><input type="file" accept=".zip,application/zip" ref={file} data-testid="skill-import-file" required /></Field>
            <Field label="Source URL (optional)" grow><input name="source_url" type="url" placeholder="https://agentskills.io/…" /></Field>
          </>
        )}
        <Field label="Licence (if SKILL.md has none)"><input name="licence" placeholder="MIT, Apache-2.0" /></Field>
        <button>Import for review</button>
      </Form>
      <Err error={act.error} id="skill-import-error" />
    </Card>
  );
}
