// SkoWork admin console shell (control-plane/admin-console): sign-in, and the views the admin's roles allow
// (the server returns them with /me; design D6). Views are addressed as #/<view>.
import { useEffect, useState, type ReactNode } from 'react';
import { api, ApiError, init, signIn, signOut } from './auth.ts';
import { AccessRecords, Audit, Individual } from './views/audit.tsx';
import { Connections } from './views/connections.tsx';
import { Dashboards } from './views/dashboards.tsx';
import { Fleet } from './views/fleet.tsx';
import { Identity } from './views/identity.tsx';
import { Knowledge } from './views/knowledge.tsx';
import { Mcp } from './views/mcp.tsx';
import { Models } from './views/models.tsx';
import { Policy } from './views/policy.tsx';
import { Remote } from './views/remote.tsx';
import { Skills } from './views/skills.tsx';
import { Asker, Notices } from './ui.tsx';

export interface Me {
  user_name: string;
  display_name: string;
  groups: string[];
  admin_roles: string[];
  views: string[];
}

const TITLES: Record<string, string> = {
  dashboards: 'Dashboards',
  policy: 'Policies',
  models: 'Models & quotas',
  connections: 'LLM connections',
  knowledge: 'Team knowledge',
  identity: 'Identity',
  remote: 'Remote tasks',
  mcp: 'Connectors',
  skills: 'Skill review',
  fleet: 'Fleet',
  audit: 'Audit log',
  individual: 'Individual access',
  'access-records': 'Access records',
};

const PURPOSES: Record<string, string> = {
  dashboards: 'Adoption and usage of SkoWork per Group.',
  identity: 'Who gets tokens, who belongs to which Group, and who holds an administrator role.',
  fleet: 'Installed clients, their versions and policies. Revoke a machine or a user, or set the minimum version.',
  policy: 'Write, publish, roll out and roll back the policy of the organization and of each Group.',
  models: 'Which models each Group may use, and its token quotas.',
  connections: 'The LLM servers the gateway sends requests to, and the models they offer.',
  mcp: 'MCP servers and API connectors that the agent may use, and who may use them.',
  skills: 'Review Skills submitted for the whole organization, import public Skills, and revoke published ones.',
  knowledge: 'Knowledge items users have shared with their Groups.',
  remote: 'Which systems may hand tasks to users, their limits and callback addresses.',
  audit: 'Every administrative action and refusal, searchable and exportable.',
  individual: 'Look up one user’s activity, with a recorded reason.',
  'access-records': 'Who looked at individual data, and why.',
};

const GROUPS: { title: string; views: string[] }[] = [
  { title: 'Overview', views: ['dashboards'] },
  { title: 'Users and devices', views: ['identity', 'fleet'] },
  { title: 'Policies', views: ['policy', 'models'] },
  { title: 'AI services', views: ['connections', 'mcp', 'skills', 'knowledge'] },
  { title: 'Automation', views: ['remote'] },
  { title: 'Compliance', views: ['audit', 'individual', 'access-records'] },
];

function render(view: string, me: Me, onQueue: (n: number) => void): ReactNode {
  switch (view) {
    case 'dashboards': return <Dashboards />;
    case 'policy': return <Policy me={me} />;
    case 'models': return <Models />;
    case 'connections': return <Connections />;
    case 'knowledge': return <Knowledge />;
    case 'identity': return <Identity />;
    case 'remote': return <Remote />;
    case 'mcp': return <Mcp />;
    case 'skills': return <Skills me={me} onQueue={onQueue} />;
    case 'fleet': return <Fleet me={me} />;
    case 'audit': return <Audit />;
    case 'individual': return <Individual />;
    case 'access-records': return <AccessRecords />;
    default: return null;
  }
}

const hashView = () => location.hash.replace(/^#\/?/, '').split(/[/?]/)[0] ?? '';

export function App() {
  const [state, setState] = useState<'loading' | 'signed-out' | 'ready' | 'forbidden' | 'error'>('loading');
  const [me, setMe] = useState<Me | null>(null);
  const [error, setError] = useState('');
  const [view, setView] = useState(hashView());
  const [pending, setPending] = useState(0);

  useEffect(() => {
    if (!me?.views.includes('skills')) return;
    api<unknown[]>('GET', '/api/v1/admin/skills/queue').then((q) => setPending(q.length), () => {});
  }, [me]);

  useEffect(() => {
    const onHash = () => setView(hashView());
    addEventListener('hashchange', onHash);
    (async () => {
      try {
        if (!(await init())) return setState('signed-out');
        setMe(await api<Me>('GET', '/api/v1/admin/me'));
        setState('ready');
      } catch (e) {
        if (e instanceof ApiError && e.status === 403) return setState('forbidden');
        setError((e as Error).message);
        setState('error');
      }
    })();
    return () => removeEventListener('hashchange', onHash);
  }, []);

  if (state === 'loading') return <Centered>Loading…</Centered>;
  if (state === 'signed-out')
    return (
      <Centered>
        <h1>SkoWork admin</h1>
        <button className="primary" data-testid="sign-in" onClick={() => void signIn()}>Sign in with organization account</button>
      </Centered>
    );
  if (state === 'forbidden')
    return (
      <Centered>
        <p data-testid="forbidden">Your account has no administrator role, so the admin console is not available.</p>
        <button onClick={() => void signOut()}>Sign out</button>
      </Centered>
    );
  if (state === 'error') return <Centered>Something went wrong: {error}</Centered>;

  const views = me!.views;
  const current = views.includes(view) ? view : (views[0] ?? '');
  const groups = GROUPS.map((g) => ({ ...g, views: g.views.filter((v) => views.includes(v)) })).filter((g) => g.views.length);
  const other = views.filter((v) => !GROUPS.some((g) => g.views.includes(v)));
  if (other.length) groups.push({ title: 'More', views: other });
  return (
    <div className="shell">
      <header>
        <strong className="brand">SkoWork admin</strong>
        <span className="me" data-testid="me">
          <span>{me!.display_name} <small>({me!.admin_roles.join(', ')})</small></span>
          <button onClick={() => void signOut()}>Sign out</button>
        </span>
      </header>
      <nav data-testid="nav" aria-label="Views">
        {groups.map((g) => (
          <div key={g.title} className="nav-group">
            <h2>{g.title}</h2>
            {g.views.map((v) => (
              <a key={v} href={`#/${v}`} className={v === current ? 'active' : ''} aria-current={v === current ? 'page' : undefined} data-testid={`nav-${v}`}>
                {TITLES[v] ?? v}
                {v === 'skills' && pending > 0 && <span className="count" data-testid="nav-skills-count" aria-label={`${pending} waiting for review`}>{pending}</span>}
              </a>
            ))}
          </div>
        ))}
      </nav>
      <Notices />
      <Asker />
      <main data-testid={`view-${current}`}>
        {current ? (
          <>
            <div className="view-head">
              <div>
                <h1>{TITLES[current] ?? current}</h1>
                {PURPOSES[current] && <p className="muted">{PURPOSES[current]}</p>}
              </div>
            </div>
            {render(current, me!, setPending)}
          </>
        ) : <p>No views are available for your roles yet.</p>}
      </main>
    </div>
  );
}

function Centered({ children }: { children: ReactNode }) {
  return <div className="centered">{children}</div>;
}
