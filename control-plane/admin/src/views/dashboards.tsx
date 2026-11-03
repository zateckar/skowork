// Aggregate usage dashboards (control-plane/admin-console Aggregate Usage Dashboards): figures per Group only;
// Groups and cells with fewer than 5 users are suppressed by the server (design D7).
import { useState } from 'react';
import { Card, Err, useLoad } from '../ui.tsx';

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
const hidden = (n: number) => (n ? <li className="muted">{n} more below the 5-user floor</li> : null);

export function Dashboards() {
  const [days, setDays] = useState(7);
  const d = useLoad<{ since: string; until: string; k: number; groups: GroupRow[] }>(`/api/v1/admin/dashboards?days=${days}`);
  const chats = useLoad<{ chats: number; direct_chats: number; team_chats: number; messages: number; messages_last_day: number }>('/api/v1/admin/messaging/counts');
  return (
    <>
      <p className="row">
        <label className="row">Period <select data-testid="dash-days" value={days} onChange={(e) => setDays(Number(e.target.value))}>
          {[1, 7, 30, 90].map((n) => <option key={n} value={n}>last {n} day{n > 1 ? 's' : ''}</option>)}
        </select></label>
        <span className="muted">Figures are per Group. Groups and figures with fewer than {d.data?.k ?? 5} users are not shown.</span>
      </p>
      <Err error={d.error} />
      <div className="grid">
        {chats.data && (
          <Card title="Messaging" id="dash-messaging">
            <dl>
              <dt>Chats</dt><dd data-testid="dash-chats">{chats.data.chats} ({chats.data.direct_chats} direct, {chats.data.team_chats} team)</dd>
              <dt>Messages kept</dt><dd data-testid="dash-messages">{chats.data.messages}</dd>
              <dt>Messages in the last day</dt><dd>{chats.data.messages_last_day}</dd>
            </dl>
          </Card>
        )}
        {d.data?.groups.map((g) => (
          <Card key={g.group} title={g.group} id={`dash-${g.group}`}>
            {g.suppressed ? (
              <p className="muted" data-testid={`dash-suppressed-${g.group}`}>Fewer than {d.data!.k} active users: suppressed.</p>
            ) : (
              <dl>
                <dt>Active users</dt><dd data-testid={`dash-users-${g.group}`}>{g.active_users}</dd>
                <dt>Active installations</dt><dd>{g.active_installations ?? '—'}</dd>
                <dt>Runs</dt>
                <dd data-testid={`dash-runs-${g.group}`}>{g.runs ? `${g.runs.total} (failed ${g.runs.failed}, error rate ${pct(g.runs.error_rate)})` : '—'}</dd>
                <dt>Client errors</dt><dd>{g.client_errors ?? '—'}</dd>
                <dt>Tokens</dt>
                <dd>
                  {g.tokens ? `${g.tokens.used.toLocaleString()}${g.tokens.budget_per_user ? ` of ${g.tokens.budget_per_user.toLocaleString()} per user and ${g.tokens.budget_period} (${pct(g.tokens.quota_used ?? 0)} of the Group's quota)` : ''}` : '—'}
                </dd>
                <dt>Tool usage</dt>
                <dd><ul>{g.tools?.cells.map((t) => <li key={t.tool}>{t.tool}: {t.calls} calls, {t.errors} errors</li>)}{hidden(g.tools?.suppressed ?? 0)}</ul></dd>
                <dt>Versions</dt>
                <dd><ul>{g.versions?.cells.map((v) => <li key={v.version}>{v.version}: {v.installations}</li>)}{hidden(g.versions?.suppressed ?? 0)}</ul></dd>
              </dl>
            )}
          </Card>
        ))}
      </div>
    </>
  );
}
