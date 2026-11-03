// Team knowledge (control-plane/admin-console Team Knowledge Administration): the items users shared with Groups
// through control-plane/team-knowledge, by Group, and removal with a reason.
import { useState } from 'react';
import { api } from '../auth.ts';
import { Card, Err, Field, Form, Note, confirmed, qs, time, useAction, useLoad } from '../ui.tsx';

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
  const [removing, setRemoving] = useState<number | null>(null);
  const act = useAction();

  const remove = (it: Item, f: Record<string, string>) => {
    if (!confirmed(`Remove "${it.title}" shared by ${it.owner} with ${it.groups.join(', ')}? Its text is deleted from team knowledge.`)) return;
    void act.run(async () => {
      await api('DELETE', `/api/v1/admin/knowledge/items/${it.id}`, { reason: f.reason });
      setRemoving(null);
      list.reload();
    }, `Removed "${it.title}".`);
  };

  return (
    <Card title="Team knowledge" id="knowledge">
      <p className="muted">Documents, folders and notes that users shared with their Groups. Users share only with Groups that policy.knowledge.sharing.groups permits.</p>
      <Field label="Group">
        <select data-testid="knowledge-group" value={group} onChange={(e) => setGroup(e.target.value)}>
          <option value="">all Groups</option>
          {list.data?.groups.map((g) => <option key={g.group} value={g.group}>{g.group} ({g.items} items, {size(g.size)})</option>)}
        </select>
      </Field>
      <Err error={list.error} />
      {list.data?.items.length === 0 && <p className="empty">Nothing is shared{group ? ` with ${group}` : ''}.</p>}
      <ul className="entries">
        {list.data?.items.map((it) => (
          <li key={it.id} data-testid={`knowledge-item-${it.item_id}`}>
            <div>
              <strong>{it.title}</strong>
              <div className="muted small">
                {it.owner} · {it.kind} · {size(it.size)} · {it.groups.join(', ')} · updated {time(it.updated_at)}{it.embedded ? '' : ' · full text only'}
              </div>
            </div>
            {removing === it.id ? (
              <Form id={`knowledge-remove-${it.item_id}`} onSubmit={(f) => remove(it, f)}>
                <Field label="Reason" grow><input name="reason" required /></Field>
                <button className="danger">Remove</button>
                <button type="button" onClick={() => setRemoving(null)}>Cancel</button>
              </Form>
            ) : (
              <button className="danger" onClick={() => setRemoving(it.id)} data-testid={`knowledge-remove-open-${it.item_id}`}>Remove…</button>
            )}
          </li>
        ))}
      </ul>
      <Err error={act.error} id="knowledge-error" />
      <Note text={act.note} id="knowledge-note" />
    </Card>
  );
}
