// Workbench admin console shell (control-plane/admin-console): sign-in, and the views the admin's roles allow
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
  mcp: 'MCP catalog',
  skills: 'Skill review',
  fleet: 'Fleet',
  audit: 'Audit log',
  individual: 'Individual access',
  'access-records': 'Access records',
};

function render(view: string, me: Me): ReactNode {
  switch (view) {
    case 'dashboards': return <Dashboards />;
    case 'policy': return <Policy me={me} />;
    case 'models': return <Models />;
    case 'connections': return <Connections />;
    case 'knowledge': return <Knowledge />;
    case 'identity': return <Identity />;
    case 'remote': return <Remote />;
    case 'mcp': return <Mcp />;
    case 'skills': return <Skills me={me} />;
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
        <h1>Workbench admin</h1>
        <button data-testid="sign-in" onClick={() => void signIn()}>Sign in with organization account</button>
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
  return (
    <div className="shell">
      <header>
        <strong>Workbench admin</strong>
        <nav data-testid="nav">
          {views.map((v) => (
            <a key={v} href={`#/${v}`} className={v === current ? 'active' : ''} data-testid={`nav-${v}`}>{TITLES[v] ?? v}</a>
          ))}
        </nav>
        <span className="me" data-testid="me">
          {me!.display_name} ({me!.admin_roles.join(', ')}) <button onClick={() => void signOut()}>Sign out</button>
        </span>
      </header>
      <main data-testid={`view-${current}`}>{current ? render(current, me!) : <p>No views are available for your roles yet.</p>}</main>
    </div>
  );
}

function Centered({ children }: { children: ReactNode }) {
  return <div className="centered">{children}</div>;
}
