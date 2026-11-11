// Team knowledge (control-plane/admin-console Team Knowledge Administration): the items users shared with Groups
// through control-plane/team-knowledge as records, filtered by Group; removal asks for a reason
// (redesign-admin-console-records D3). Publications (improve-skowork-and-knowledge-control design D9) are listed
// beside them: an administrator inspects the exact published revision, removes it with a reason, and resolves a
// removal so its owner may publish again.
import { useState } from 'react';
import { api } from '../auth.ts';
import { ask, Chips, Dialog, notify, qs, Records, time, useDialog, useLoad } from '../ui.tsx';

interface Item {
  id: number;
  item_id: string;
  title: string;
  kind: string;
  owner: string;
  size: number;
  groups: string[];
  chunks: number;
  embedded: boolean;
  updated_at: string;
}

interface Publication {
  publication: string;
  owner: string | null;
  revision: number;
  state: 'published' | 'withdrawn' | 'removed';
  title: string | null;
  groups: string[];
  size: number;
  entries: number;
  updated_at: string | null;
}

interface PublishedRevision {
  publication: string;
  owner: string | null;
  revision: number;
  state: Publication['state'];
  title: string | null;
  groups: string[];
  updated_at: string | null;
  removal: { reason: string; removed_by: string; removed_at: string } | null;
  entries: { entry_id: string; text: string; source_kinds: string[]; evidence: { id: string; title: string; text: string }[] }[];
}

const size = (n: number) => (n >= 1024 * 1024 ? `${(n / 1024 / 1024).toFixed(1)} MB` : n >= 1024 ? `${Math.round(n / 1024)} KB` : `${n} B`);
const STATE = { published: 'ok', withdrawn: 'pending', removed: 'failing' } as const;

export function Knowledge() {
  return (
    <>
      <SharedItems />
      <Publications />
    </>
  );
}

function SharedItems() {
  const [group, setGroup] = useState('');
  const list = useLoad<{ groups: { group: string; items: number; size: number }[]; items: Item[] }>(`/api/v1/admin/knowledge/items?${qs({ group })}`);

  const remove = async (it: Item) => {
    const reason = await ask({
      title: `Remove “${it.title}”?`,
      body: <p>Shared by {it.owner} with {it.groups.join(', ')}. Its text is deleted from team knowledge; {it.owner} keeps their own copy.</p>,
      action: 'Remove item',
      reason: 'required',
      reasonLabel: 'Reason (kept in the audit log)',
    });
    if (reason === null) return;
    await api('DELETE', `/api/v1/admin/knowledge/items/${it.id}`, { reason });
    list.reload();
    notify(`Removed “${it.title}”.`, 'knowledge-note');
  };

  return (
    <Records
      title="Team knowledge"
      id="knowledge"
      sub="Documents, folders and notes that users shared with their Groups, as policy.knowledge.sharing.groups permits."
      items={list.data?.items}
      error={list.error}
      text={(it) => `${it.title} ${it.owner} ${it.kind} ${it.groups.join(' ')}`}
      tools={
        <select aria-label="Group" data-testid="knowledge-group" value={group} onChange={(e) => setGroup(e.target.value)}>
          <option value="">All Groups</option>
          {list.data?.groups.map((g) => <option key={g.group} value={g.group}>{g.group} ({g.items} items, {size(g.size)})</option>)}
        </select>
      }
      head={<tr><th>Item</th><th>Shared with</th><th>Search</th><th /></tr>}
      empty={`Nothing is shared${group ? ` with ${group}` : ''}.`}
      row={(it) => (
        <tr key={it.id} data-testid={`knowledge-item-${it.item_id}`}>
          <td><b>{it.title}</b><small>{it.owner} · {it.kind} · {size(it.size)} · {it.groups.join(', ')} · updated {time(it.updated_at)}</small></td>
          <td><Chips values={it.groups} /></td>
          <td>{it.embedded ? `${it.chunks} passages` : <span className="muted">full text only</span>}</td>
          <td className="acts"><button className="link danger" data-testid={`knowledge-remove-open-${it.item_id}`} onClick={() => void remove(it)}>Remove</button></td>
        </tr>
      )}
    />
  );
}

function Publications() {
  const list = useLoad<{ publications: Publication[] }>('/api/v1/admin/knowledge/publications');
  const [open, inspect, close] = useDialog<string>();

  const remove = async (p: Publication) => {
    const reason = await ask({
      title: `Remove “${p.title}”?`,
      body: <p>Published by {p.owner} to {p.groups.join(', ')}. Its entries and excerpts leave team search now, and {p.owner} sees that it was removed and why. It stays removed until you resolve it.</p>,
      action: 'Remove publication',
      reason: 'required',
      reasonLabel: 'Reason (shown to the owner and kept in the audit log)',
    });
    if (reason === null) return;
    await api('DELETE', `/api/v1/admin/knowledge/publications/${p.publication}`, { reason });
    list.reload();
    notify(`Removed “${p.title}”.`, 'knowledge-note');
  };
  const resolve = async (p: Publication) => {
    const ok = await ask({
      title: 'Resolve this removal?',
      body: <p>{p.owner} may publish this knowledge again. Nothing is restored: they must confirm a new publication.</p>,
      action: 'Resolve',
      danger: false,
    });
    if (ok === null) return;
    await api('POST', `/api/v1/admin/knowledge/publications/${p.publication}/resolve`, {});
    list.reload();
    notify('Resolved the removal.', 'knowledge-note');
  };

  return (
    <>
      <Records
        title="Knowledge publications"
        id="publications"
        sub="Selected knowledge that users published to their Groups, with its approved excerpts. Each change is a new revision."
        items={list.data?.publications}
        error={list.error}
        text={(p) => `${p.title ?? ''} ${p.owner ?? ''} ${p.state} ${p.groups.join(' ')}`}
        head={<tr><th>Publication</th><th>Published to</th><th>State</th><th /></tr>}
        empty="Nothing is published."
        row={(p) => (
          <tr key={p.publication} data-testid={`publication-${p.publication}`}>
            <td>
              <b>{p.title ?? <span className="muted">No current content</span>}</b>
              <small>{p.owner ?? 'unknown owner'} · revision {p.revision} · {p.entries} entries · {size(p.size)}{p.updated_at ? ` · updated ${time(p.updated_at)}` : ''}</small>
            </td>
            <td><Chips values={p.groups} /></td>
            <td><span className={`state ${STATE[p.state]}`}>{p.state}</span></td>
            <td className="acts">
              <button className="link" data-testid={`publication-inspect-${p.publication}`} onClick={() => inspect(p.publication)}>Inspect</button>
              {p.state === 'published' && (
                <button className="link danger" data-testid={`publication-remove-open-${p.publication}`} onClick={() => void remove(p)}>Remove</button>
              )}
              {p.state === 'removed' && (
                <button className="link" data-testid={`publication-resolve-${p.publication}`} onClick={() => void resolve(p)}>Resolve</button>
              )}
            </td>
          </tr>
        )}
      />
      {open && <Revision publication={open} onClose={close} />}
    </>
  );
}

/** The exact current revision: only what the owner confirmed for publication, never their private sources. */
function Revision({ publication, onClose }: { publication: string; onClose: () => void }) {
  const r = useLoad<PublishedRevision>(`/api/v1/admin/knowledge/publications/${publication}`);
  const d = r.data;
  return (
    <Dialog id="publication" title={d ? `${d.title ?? 'Publication'} · revision ${d.revision}` : 'Publication'} wide onClose={onClose}>
      {r.error && <p className="err" role="alert">{r.error}</p>}
      {!d && !r.error && <p className="muted">Loading…</p>}
      {d && (
        <>
          <div className="facts">
            <div><span>Owner</span><b>{d.owner ?? '—'}</b></div>
            <div><span>State</span><b>{d.state}</b></div>
            <div><span>Published to</span><b>{d.groups.join(', ') || '—'}</b></div>
            {d.updated_at && <div><span>Updated</span><b>{time(d.updated_at)}</b></div>}
          </div>
          {d.removal && <p data-testid="publication-removal">Removed by {d.removal.removed_by} on {time(d.removal.removed_at)}: {d.removal.reason}</p>}
          {d.entries.length === 0 && <p className="muted">This revision has no current content.</p>}
          {d.entries.map((e) => (
            <section key={e.entry_id} className="entry" data-testid={`publication-entry-${e.entry_id}`}>
              <p>{e.text}</p>
              <small className="muted">From {e.source_kinds.join(', ')}</small>
              {e.evidence.map((x) => (
                <blockquote key={x.id}><b>{x.title}</b><br />{x.text}</blockquote>
              ))}
            </section>
          ))}
        </>
      )}
    </Dialog>
  );
}
