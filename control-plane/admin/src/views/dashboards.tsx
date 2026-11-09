// Aggregate usage dashboards (control-plane/admin-console Aggregate Usage Dashboards): figures per Group only;
// Groups and cells with fewer than 5 users are suppressed by the server (design D7). The Groups are records with
// their headline figures; tool usage and client versions of a Group open in a dialog
// (redesign-admin-console-records D3).
import { useState } from 'react';
import { Card, Dialog, Err, Records, useDialog, useLoad } from '../ui.tsx';

interface Cells<T> { cells: T[]; suppressed: number }
interface GroupRow {
  group: string;
  suppressed: boolean;
  active_users?: number;
  active_installations?: number | null;
  runs?: { total: number; failed: number; cancelled: number; error_rate: number } | null;
  tools?: Cells<{ tool: string; calls: number; errors: number }>;
  client_errors?: number | null;
  tokens?: { used: number; users: number; budget_per_user: number | null; budget_period: string | null; quota_used: number | null } | null;
  versions?: Cells<{ version: string; installations: number }>;
}

const pct = (x: number) => `${(x * 100).toFixed(1)} %`;

export function Dashboards() {
  const [days, setDays] = useState(7);
  const d = useLoad<{ since: string; until: string; k: number; groups: GroupRow[] }>(`/api/v1/admin/dashboards?days=${days}`);
  const chats = useLoad<{ chats: number; direct_chats: number; team_chats: number; messages: number; messages_last_day: number }>('/api/v1/admin/messaging/counts');
  const [detail, openDetail, closeDetail] = useDialog<GroupRow>();
  const k = d.data?.k ?? 5;
  return (
    <>
      {chats.data && (
        <Card title="Messaging" id="dash-messaging" sub="Counts only; message content is never shown here.">
          <div className="facts">
            <div><span>Chats</span><b data-testid="dash-chats">{chats.data.chats} ({chats.data.direct_chats} direct, {chats.data.team_chats} team)</b></div>
            <div><span>Messages kept</span><b data-testid="dash-messages">{chats.data.messages}</b></div>
            <div><span>Messages in the last day</span><b>{chats.data.messages_last_day}</b></div>
          </div>
        </Card>
      )}
      <Err error={d.error} />
      <Records
        title="Usage by Group"
        id="dash-groups"
        sub={`Figures are per Group. Groups and figures with fewer than ${k} users are not shown.`}
        items={d.data?.groups}
        text={(g) => g.group}
        tools={
          <select aria-label="Period" data-testid="dash-days" value={days} onChange={(e) => setDays(Number(e.target.value))}>
            {[1, 7, 30, 90].map((n) => <option key={n} value={n}>Last {n} day{n > 1 ? 's' : ''}</option>)}
          </select>
        }
        head={<tr><th>Group</th><th>Active users</th><th>Installations</th><th>Runs</th><th>Client errors</th><th>Tokens</th><th /></tr>}
        empty="No Group has activity in this period."
        row={(g) => g.suppressed ? (
          <tr key={g.group} data-testid={`dash-${g.group}`}>
            <td><b>{g.group}</b></td>
            <td colSpan={6} className="muted" data-testid={`dash-suppressed-${g.group}`}>Fewer than {k} active users: suppressed.</td>
          </tr>
        ) : (
          <tr key={g.group} data-testid={`dash-${g.group}`}>
            <td><b>{g.group}</b></td>
            <td data-testid={`dash-users-${g.group}`}>{g.active_users}</td>
            <td>{g.active_installations ?? '—'}</td>
            <td data-testid={`dash-runs-${g.group}`}>{g.runs ? `${g.runs.total} (failed ${g.runs.failed}, error rate ${pct(g.runs.error_rate)})` : '—'}</td>
            <td>{g.client_errors ?? '—'}</td>
            <td>
              {g.tokens ? g.tokens.used.toLocaleString() : '—'}
              {g.tokens?.budget_per_user ? <small>{pct(g.tokens.quota_used ?? 0)} of the quota ({g.tokens.budget_per_user.toLocaleString()} per user and {g.tokens.budget_period})</small> : null}
            </td>
            <td className="acts"><button className="link" data-testid={`dash-open-${g.group}`} onClick={() => openDetail(g)}>Tools and versions</button></td>
          </tr>
        )}
      />
      {detail && (
        <Dialog id="dash-detail" title={`Tools and versions in ${detail.group}`} wide onClose={closeDetail}>
          <div className="two">
            <table>
              <thead><tr><th>Tool</th><th>Calls</th><th>Errors</th></tr></thead>
              <tbody>
                {detail.tools?.cells.map((t) => <tr key={t.tool}><td><code>{t.tool}</code></td><td>{t.calls}</td><td>{t.errors}</td></tr>)}
                {!!detail.tools?.suppressed && <tr><td colSpan={3} className="muted">{detail.tools.suppressed} more below the {k}-user floor</td></tr>}
                {!detail.tools?.cells.length && !detail.tools?.suppressed && <tr><td colSpan={3} className="empty">No tool calls.</td></tr>}
              </tbody>
            </table>
            <table>
              <thead><tr><th>Client version</th><th>Installations</th></tr></thead>
              <tbody>
                {detail.versions?.cells.map((v) => <tr key={v.version}><td>{v.version}</td><td>{v.installations}</td></tr>)}
                {!!detail.versions?.suppressed && <tr><td colSpan={2} className="muted">{detail.versions.suppressed} more below the {k}-user floor</td></tr>}
              </tbody>
            </table>
          </div>
        </Dialog>
      )}
    </>
  );
}
