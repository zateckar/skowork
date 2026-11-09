// Team knowledge (control-plane/admin-console Team Knowledge Administration): the items users shared with Groups
// through control-plane/team-knowledge as records, filtered by Group; removal asks for a reason
// (redesign-admin-console-records D3).
import { useState } from 'react';
import { api } from '../auth.ts';
import { ask, Chips, notify, qs, Records, time, useLoad } from '../ui.tsx';

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

const size = (n: number) => (n >= 1024 * 1024 ? `${(n / 1024 / 1024).toFixed(1)} MB` : n >= 1024 ? `${Math.round(n / 1024)} KB` : `${n} B`);

export function Knowledge() {
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
