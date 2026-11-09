// Skill review queue (control-plane/admin-console Skill Review Queue) over control-plane/skill-registry: the submitted
// org versions as records; each opens in a review dialog with its scan results and the diff against the previous
// version, to approve or reject with a comment. Public import for platform-admins is a dialog too; revocation asks for
// a reason (redesign-admin-console-records D3).
import { useEffect, useRef, useState } from 'react';
import type { Me } from '../App.tsx';
import { api, download, upload } from '../auth.ts';
import { ask, Dialog, Err, Field, notify, qs, Records, time, useAction, useDialog, useLoad } from '../ui.tsx';

const S = '/api/v1/admin/skills';

interface Version {
  id: number; name: string; version: string; scope: string; audience: string[]; state: string; author: string; description: string;
  risk: string; network: string; tools: string[]; requires_mcp: string[]; findings: number; source_url: string | null; licence: string | null;
  review_comment: string | null; reviewed_by: string | null; created_at: string; revoke_reason: string | null; self_approved: boolean;
}
interface Finding { kind: string; severity: string; file: string; line: number; rule: string; message: string }
interface Detail extends Version {
  scan: { findings: Finding[]; commands: { command: string; file: string; line: number }[]; urls: { url: string; file: string; line: number }[] };
  body: string;
  files: { path: string; size: number }[];
  diff: { against: string; files: { path: string; status: string; lines?: string[]; note?: string }[] } | null;
  approval: 'review' | 'self-approval' | 'self-review-blocked';
}

const SCOPES: Record<string, string> = { shared: 'Shared with users', group: 'Groups', org: 'Organization' };
const same = (a: string, b: string) => a.toLowerCase() === b.toLowerCase();

export function Skills({ me, onQueue }: { me: Me; onQueue?: (n: number) => void }) {
  const queue = useLoad<Version[]>(`${S}/queue`);
  const all = useLoad<Version[]>(S);
  const [open, setOpen, close] = useDialog<number>();
  const [importing, openImport, closeImport] = useDialog();
  const reload = () => (queue.reload(), all.reload());
  const admin = me.admin_roles.includes('platform-admin');
  const reviewer = admin || me.admin_roles.includes('skill-reviewer');
  useEffect(() => void (queue.data && onQueue?.(queue.data.length)), [queue.data, onQueue]);

  const revoke = async (v: Version) => {
    const reason = await ask({
      title: `Revoke ${v.name} ${v.version}?`,
      body: <p>Clients remove it at their next sync. Users who need it again must wait for a new version to be approved.</p>,
      action: 'Revoke',
      reason: 'required',
    });
    if (reason === null) return;
    await api('POST', `${S}/${encodeURIComponent(v.name)}/revoke`, { version: v.version, reason });
    reload();
    notify(`${v.name} ${v.version} revoked.`, 'skill-note');
  };

  return (
    <>
      <Records
        title="Waiting for review"
        id="skill-queue"
        sub="Skill versions submitted for the whole organization. Approving signs them for distribution."
        items={queue.data}
        error={queue.error}
        text={(v) => `${v.name} ${v.version} ${v.author} ${v.description}`}
        tools={<button data-testid="skill-refresh" onClick={reload}>Refresh</button>}
        add={admin && <button className="primary" data-testid="skill-import-open" onClick={() => openImport(true)}>Import a public Skill</button>}
        empty={<span data-testid="skill-queue-empty">No Skill versions are waiting for review. Versions submitted for the whole organization appear here.</span>}
        head={<tr><th>Skill</th><th>Author</th><th>Declared</th><th>Findings</th><th>Submitted</th><th /></tr>}
        row={(v) => (
          <tr key={v.id} data-testid={`skill-${v.name}-${v.version}`}>
            <td><b>{v.name}</b> {v.version}<small>{v.description}</small>{v.licence && <small>Imported from {v.source_url ?? 'an uploaded package'} ({v.licence})</small>}</td>
            <td>{v.author}{same(v.author, me.user_name) && <small>you</small>}</td>
            <td><small>risk {v.risk}, tools {v.tools.join(', ') || 'default'}{v.requires_mcp.length ? `, MCP ${v.requires_mcp.join(', ')}` : ''}</small></td>
            <td>{v.findings ? <span className="state failing">{v.findings}</span> : <span className="muted">none</span>}</td>
            <td>{time(v.created_at)}</td>
            <td className="acts"><button className="primary" data-testid={`skill-open-${v.id}`} onClick={() => setOpen(v.id)}>Review</button></td>
          </tr>
        )}
      />

      <Records
        title="All Skill versions"
        id="skill-all"
        items={all.data}
        error={all.error}
        text={(v) => `${v.name} ${v.version} ${v.author} ${v.state}`}
        head={<tr><th>Skill</th><th>Shared with</th><th>State</th><th>Author</th><th /></tr>}
        empty="No Skills have been shared yet."
        row={(v) => (
          <tr key={v.id} data-testid={`skill-row-${v.name}-${v.version}`}>
            <td><b>{v.name}</b> {v.version}</td>
            <td>{SCOPES[v.scope] ?? v.scope}{v.audience.length ? `: ${v.audience.join(', ')}` : ''}</td>
            <td>
              <span data-testid={`skill-state-${v.name}-${v.version}`}><span className={`state ${v.state}`}>{v.state}</span>{v.revoke_reason ? ` (${v.revoke_reason})` : ''}{v.review_comment ? ` — ${v.review_comment}` : ''}</span>
              {v.self_approved && <small>self-approved</small>}
            </td>
            <td>{v.author}</td>
            <td className="acts">
              {reviewer && v.state === 'published' && <button className="link danger" data-testid={`skill-revoke-${v.name}-${v.version}`} onClick={() => void revoke(v)}>Revoke</button>}
            </td>
          </tr>
        )}
      />

      {open !== null && <Review key={open} id={open} me={me} onClose={close} onDecided={(note) => (close(), reload(), notify(note, 'skill-note'))} />}
      {importing && <Import onClose={closeImport} onDone={reload} />}
    </>
  );
}

function Review({ id, me, onDecided, onClose }: { id: number; me: Me; onDecided: (note: string) => void; onClose: () => void }) {
  const d = useLoad<Detail>(`${S}/versions/${id}`);
  const act = useAction();
  const [comment, setComment] = useState('');
  const v = d.data;
  const self = v?.approval === 'self-approval';
  const blocked = v?.approval === 'self-review-blocked';
  const decide = (what: 'approve' | 'reject') =>
    act.run(async () => {
      await api('POST', `${S}/versions/${id}/${what}`, { comment });
      onDecided(`${v!.name} ${v!.version} ${what === 'approve' ? (self ? 'approved as a self-approval and signed' : 'approved and signed') : 'rejected'}.`);
    });
  return (
    <Dialog id="skill-review" title={v ? `Review ${v.name} ${v.version}` : 'Review'} wide onClose={onClose}>
      <Err error={d.error} id="skill-detail-error" />
      {v && (
        <div className="stack" data-testid="skill-detail">
          <div className="card-head">
            <div>
              <p>{v.description}</p>
              <p className="muted" data-testid="skill-declared">
                Declared risk <b>{v.risk}</b>, network {v.network}, tools {v.tools.join(', ') || 'default (read-class)'}
                {v.requires_mcp.length ? `, requires MCP ${v.requires_mcp.join(', ')}` : ''}
              </p>
              <p className="muted">Submitted by {v.author}{same(v.author, me.user_name) ? ' (you)' : ''} on {time(v.created_at)}</p>
              {v.licence && (
                <p data-testid="skill-source">
                  {v.source_url ? <>Imported from <a href={v.source_url} rel="noreferrer noopener" target="_blank">{v.source_url}</a></> : 'Imported as an uploaded package'} under {v.licence}
                </p>
              )}
            </div>
            <button type="button" onClick={() => void act.run(() => download(`${S}/versions/${id}/content`))}>Download package</button>
          </div>

          <div className={`callout ${v.scan.findings.length ? 'danger' : ''}`}>
            <h3>Scan findings</h3>
            <ul data-testid="skill-findings">
              {v.scan.findings.length === 0 && <li>None.</li>}
              {v.scan.findings.map((f, i) => <li key={i}><b>{f.severity}</b> {f.kind} ({f.rule}) — {f.file}:{f.line}: {f.message}</li>)}
            </ul>
          </div>
          <div className="two">
            <div><h3>Script commands</h3><p data-testid="skill-commands">{v.scan.commands.map((c) => c.command).join(', ') || 'None.'}</p></div>
            <div><h3>Network endpoints</h3><p data-testid="skill-urls">{v.scan.urls.map((u) => u.url).join(', ') || 'None.'}</p></div>
          </div>
          <div>
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
          </div>

          <div>
            <h3>Decision</h3>
            {self && (
              <p className="callout warn" data-testid="skill-self-approval">
                You submitted this version, and you are the only skill-reviewer or platform-admin. You may approve it, but it will be
                recorded as a <b>self-approval</b> on the version and in the audit log. Explain in the comment why it is safe to publish.
              </p>
            )}
            {blocked && (
              <p className="callout warn" data-testid="skill-self-blocked">
                You submitted this version, so another skill-reviewer or platform-admin must approve it. You can still reject it.
              </p>
            )}
            <Field label={self ? 'Comment (required)' : 'Comment (required to reject)'} grow>
              <textarea data-testid="skill-comment" value={comment} onChange={(e) => setComment(e.target.value)} rows={2} />
            </Field>
            <Err error={act.error} id="skill-review-error" />
            <div className="actions decision">
              <button type="button" className="danger" data-testid="skill-reject" onClick={() => void decide('reject')}>Reject</button>
              <button type="button" className="primary" data-testid="skill-approve" disabled={blocked || (self && !comment.trim())} onClick={() => void decide('approve')}>Approve and sign</button>
            </div>
          </div>
        </div>
      )}
    </Dialog>
  );
}

function Import({ onClose, onDone }: { onClose: () => void; onDone: () => void }) {
  const file = useRef<HTMLInputElement>(null);
  const [source, setSource] = useState<'url' | 'file'>('url');
  return (
    <Dialog id="skill-import-form" title="Import a public Skill" submit="Import for review" onClose={onClose} noteId="skill-note"
      intro={<p>The package is scanned and enters the review queue like any other submission. It counts as yours for the review.</p>}
      onSubmit={async (f) => {
        let v: Version;
        if (source === 'url') {
          v = await api<Version>('POST', `${S}/import?${qs({ url: f.url, licence: f.licence })}`);
        } else {
          const pkg = file.current?.files?.[0];
          if (!pkg) throw new Error('Choose the Skill package (zip).');
          v = await upload<Version>(`${S}/import?${qs({ source_url: f.source_url, licence: f.licence })}`, pkg);
        }
        onDone();
        return `${v.name} ${v.version} entered the review queue.`;
      }}>
      <Field label="Source">
        <select data-testid="skill-import-source" value={source} onChange={(e) => setSource(e.target.value as 'url' | 'file')}>
          <option value="url">Download from a URL</option>
          <option value="file">Upload a zip file</option>
        </select>
      </Field>
      {source === 'url' ? (
        <Field label="Package URL (zip)"><input name="url" type="url" placeholder="https://…/skill.zip" required /></Field>
      ) : (
        <>
          <Field label="Skill package (zip)"><input type="file" accept=".zip,application/zip" ref={file} data-testid="skill-import-file" required /></Field>
          <Field label="Source URL (optional)"><input name="source_url" type="url" placeholder="https://agentskills.io/…" /></Field>
        </>
      )}
      <Field label="Licence" help="Needed only when SKILL.md names none."><input name="licence" placeholder="MIT, Apache-2.0" /></Field>
    </Dialog>
  );
}
