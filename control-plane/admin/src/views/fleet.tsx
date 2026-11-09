// Fleet administration (control-plane/admin-console Fleet Administration) over control-plane/fleet-registry: the
// installations filtered by version, policy version, online state and Group; revocation with a reason; minimum
// versions per channel (redesign-admin-console-records D3).
import { useState } from 'react';
import { api } from '../auth.ts';
import type { Me } from '../App.tsx';
import { ask, Chips, Dialog, Field, notify, qs, Records, time, useDialog, useLoad } from '../ui.tsx';

const F = '/api/v1/admin/fleet';
interface Inst { id: string; user_name: string; machine_name: string; version: string; channel?: string; policy_version: string | null; groups: string[]; revoked_at: string | null; connected: boolean; online: boolean; last_seen: string }

const stateOf = (i: Inst) => (i.revoked_at ? 'revoked' : i.connected ? 'connected' : i.online ? 'online' : 'offline');

export function Fleet({ me }: { me: Me }) {
  const [filter, setFilter] = useState<Record<string, string>>({});
  const list = useLoad<{ items: Inst[] }>(`${F}/installations?${qs({ page_size: 500, ...filter })}`);
  const everything = useLoad<{ items: Inst[] }>(`${F}/installations?page_size=500`);
  const mins = useLoad<{ channel: string; version: string; set_by: string; set_at: string }[]>(`${F}/min-versions`);
  const [minDlg, openMin, closeMin] = useDialog<{ channel?: string; version?: string }>();
  const manage = me.admin_roles.includes('platform-admin');
  const all = everything.data?.items ?? [];
  const distinct = (f: (i: Inst) => (string | null | undefined)[]) => [...new Set(all.flatMap(f).filter((x): x is string => !!x))].sort();
  const set = (k: string, v: string) => setFilter((f) => ({ ...f, [k]: v }));
  const reload = () => (list.reload(), everything.reload());

  const revoke = async (i: Inst) => {
    const reason = await ask({
      title: `Revoke ${i.machine_name}?`,
      body: <p>{i.user_name} is signed out on {i.machine_name}, and this installation gets no tokens until the revocation is lifted on the deny list.</p>,
      action: 'Revoke installation',
      reason: 'optional',
    });
    if (reason === null) return;
    await api('POST', `${F}/installations/${i.id}/revoke`, { reason: reason || 'revoked in admin console' });
    reload();
    notify('Installation revoked.', 'fleet-note');
  };
  const revokeUser = async (user: string) => {
    const n = all.filter((i) => i.user_name === user && !i.revoked_at).length;
    const reason = await ask({ title: `Revoke ${user} everywhere?`, body: <p>{user} is signed out on {n} installation{n === 1 ? '' : 's'} and is added to the deny list.</p>, action: 'Revoke user', reason: 'optional' });
    if (reason === null) return;
    await api('POST', `${F}/users/revoke`, { user, reason });
    reload();
    notify('User revoked on every installation.', 'fleet-note');
  };

  return (
    <>
      <Records
        title="Installations"
        id="fleet"
        items={list.data?.items}
        error={list.error}
        text={(i) => `${i.user_name} ${i.machine_name} ${i.version} ${i.groups.join(' ')} ${stateOf(i)}`}
        tools={
          <span className="actions" data-testid="fleet-filter">
            <select aria-label="Client version" name="version" value={filter.version ?? ''} onChange={(e) => set('version', e.target.value)}>
              <option value="">Any version</option>{distinct((i) => [i.version]).map((v) => <option key={v}>{v}</option>)}
            </select>
            <select aria-label="Policy version" name="policy_version" value={filter.policy_version ?? ''} onChange={(e) => set('policy_version', e.target.value)}>
              <option value="">Any policy</option>{distinct((i) => [i.policy_version]).map((v) => <option key={v}>{v}</option>)}
            </select>
            <select aria-label="Group" name="group" data-testid="fleet-group" value={filter.group ?? ''} onChange={(e) => set('group', e.target.value)}>
              <option value="">Any Group</option>{distinct((i) => i.groups).map((v) => <option key={v}>{v}</option>)}
            </select>
            <select aria-label="State" name="online" value={filter.online ?? ''} onChange={(e) => set('online', e.target.value)}>
              <option value="">Online or offline</option><option value="true">Online</option><option value="false">Offline</option>
            </select>
          </span>
        }
        head={<tr><th>User</th><th>Machine</th><th>Version</th><th>Policy</th><th>Groups</th><th>State</th><th>Last seen</th><th /></tr>}
        empty={Object.values(filter).some(Boolean) ? 'No installation matches these filters.' : 'No client has registered yet.'}
        row={(i) => (
          <tr key={i.id} data-testid={`inst-${i.id}`}>
            <td><b>{i.user_name}</b></td><td>{i.machine_name}</td><td>{i.version}</td><td>{i.policy_version ?? '—'}</td><td><Chips values={i.groups} /></td>
            <td><span className={`state ${stateOf(i)}`} data-testid={`inst-state-${i.id}`}>{stateOf(i)}</span></td>
            <td>{time(i.last_seen)}</td>
            <td className="acts">
              {manage && !i.revoked_at && (
                <>
                  <button className="link danger" data-testid={`revoke-${i.id}`} onClick={() => void revoke(i)}>Revoke</button>
                  <button className="link danger" data-testid={`revoke-user-${i.id}`} onClick={() => void revokeUser(i.user_name)}>Revoke user</button>
                </>
              )}
            </td>
          </tr>
        )}
      />
      <Records
        title="Minimum supported versions"
        id="min-versions"
        sub="Clients below the minimum of their channel are told to update."
        items={mins.data}
        error={mins.error}
        filter={false}
        text={(m) => m.channel}
        head={<tr><th>Channel</th><th>Minimum version</th><th>Set</th><th /></tr>}
        add={manage && <button className="primary" data-testid="min-version-open" onClick={() => openMin({})}>Set minimum</button>}
        empty="No minimum is set: every version may connect."
        row={(m) => (
          <tr key={m.channel}>
            <td><b>{m.channel}</b></td><td>{m.version}</td><td>{m.set_by}<small>{time(m.set_at)}</small></td>
            <td className="acts">{manage && <button className="link" onClick={() => openMin(m)}>Change</button>}</td>
          </tr>
        )}
      />
      {minDlg && (
        <Dialog id="min-version" title={minDlg.channel ? `Minimum version of ${minDlg.channel}` : 'Set a minimum version'} submit="Set minimum" onClose={closeMin} noteId="fleet-note"
          intro={<p>Installations below it are told to update at their next connection.</p>}
          onSubmit={async (f) => {
            await api('PUT', `${F}/min-versions/${encodeURIComponent(f.channel ?? '')}`, { version: f.version });
            mins.reload();
            return 'Minimum version set.';
          }}>
          <div className="two">
            <Field label="Channel">
              <input name="channel" list="channels" defaultValue={minDlg.channel ?? 'stable'} readOnly={!!minDlg.channel} required />
              <datalist id="channels">{[...new Set(['stable', 'beta', ...distinct((i) => [i.channel])])].map((c) => <option key={c} value={c} />)}</datalist>
            </Field>
            <Field label="Minimum version" help={`Installed now: ${distinct((i) => [i.version]).join(', ') || 'none'}`}>
              <input name="version" placeholder="1.2.0" defaultValue={minDlg.version} pattern="\d+\.\d+\.\d+.*" required />
            </Field>
          </div>
        </Dialog>
      )}
    </>
  );
}
