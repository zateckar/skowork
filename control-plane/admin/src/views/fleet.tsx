// Fleet administration (control-plane/admin-console Fleet Administration) over control-plane/fleet-registry: the
// installations filtered by version, policy version, online state and Group; revocation; minimum versions.
import { useState } from 'react';
import { api } from '../auth.ts';
import type { Me } from '../App.tsx';
import { Card, confirmed, Err, Field, Form, Note, qs, time, useAction, useLoad } from '../ui.tsx';

const F = '/api/v1/admin/fleet';

export function Fleet({ me }: { me: Me }) {
  const [filter, setFilter] = useState<Record<string, string>>({});
  const list = useLoad<{ items: Record<string, any>[] }>(`${F}/installations?${qs({ page_size: 200, ...filter })}`);
  const mins = useLoad<{ channel: string; version: string; set_by: string; set_at: string }[]>(`${F}/min-versions`);
  const act = useAction();
  const manage = me.admin_roles.includes('platform-admin');
  return (
    <>
      <Card title="Installations" id="fleet">
        <Form id="fleet-filter" onSubmit={(f) => setFilter(f)}>
          <Field label="Client version"><input name="version" /></Field>
          <Field label="Policy version"><input name="policy_version" /></Field>
          <Field label="Group"><input name="group" /></Field>
          <Field label="State"><select name="online"><option value="">any</option><option value="true">online</option><option value="false">offline</option></select></Field>
          <button>Filter</button>
        </Form>
        <Err error={list.error} />
        <table>
          <thead><tr><th>User</th><th>Machine</th><th>Version</th><th>Policy</th><th>Groups</th><th>State</th><th>Last seen</th><th /></tr></thead>
          <tbody>
            {list.data?.items.map((i) => (
              <tr key={i.id} data-testid={`inst-${i.id}`}>
                <td>{i.user_name}</td><td>{i.machine_name}</td><td>{i.version}</td><td>{i.policy_version}</td><td>{i.groups.join(', ')}</td>
                <td data-testid={`inst-state-${i.id}`}>{i.revoked_at ? 'revoked' : i.connected ? 'connected' : i.online ? 'online' : 'offline'}</td>
                <td>{time(i.last_seen)}</td>
                <td>
                  {manage && !i.revoked_at && (
                    <button data-testid={`revoke-${i.id}`} onClick={() => confirmed('Revoke this installation? Its user is signed out on that machine.') &&
                      void act.run(async () => (await api('POST', `${F}/installations/${i.id}/revoke`, { reason: 'revoked in admin console' }), list.reload()), 'Installation revoked.')}>
                      Revoke
                    </button>
                  )}
                </td>
              </tr>
            ))}
            {list.data?.items.length === 0 && <tr><td colSpan={8} className="empty">No installations match.</td></tr>}
          </tbody>
        </table>
        {manage && (
          <Form id="revoke-user" onSubmit={(f) => confirmed(`Revoke ${f.user} on every installation?`) && act.run(async () => (await api('POST', `${F}/users/revoke`, { user: f.user, reason: f.reason }), list.reload()), 'User revoked on every installation.')}>
            <Field label="User name"><input name="user" required /></Field>
            <Field label="Reason" grow><input name="reason" /></Field>
            <button className="danger" type="submit">Revoke user</button>
          </Form>
        )}
      </Card>
      <Card title="Minimum supported versions" id="min-versions">
        <ul className="entries">{mins.data?.map((m) => <li key={m.channel}><span>{m.channel}: {m.version}</span> <small>{m.set_by}, {time(m.set_at)}</small></li>)}{mins.data?.length === 0 && <li className="empty">No minimum set.</li>}</ul>
        {manage && (
          <Form id="min-version" onSubmit={(f) => act.run(async () => (await api('PUT', `${F}/min-versions/${encodeURIComponent(f.channel ?? '')}`, { version: f.version }), mins.reload()), 'Minimum version set.')}>
            <Field label="Channel"><input name="channel" defaultValue="stable" /></Field>
            <Field label="Minimum version"><input name="version" placeholder="1.2.0" required /></Field>
            <button>Set minimum</button>
          </Form>
        )}
      </Card>
      <Err error={act.error} id="fleet-error" />
      <Note text={act.note} id="fleet-note" />
    </>
  );
}
