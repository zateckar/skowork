// dev/graph-mock command line: starts the Graph surface (with the OAuth endpoints) and the control API and prints one
// JSON line with their URLs when ready.
import { parseArgs } from 'node:util';
import { GraphMock } from './mock.ts';

const HELP = `dev/graph-mock - a local Microsoft Graph v1.0 subset for Workbench development and tests. It holds only fixture data and never delivers mail.

Usage: graph-mock [options]
  --port <n>          Graph and OAuth port (default 4810; 0 picks a free port)
  --control-port <n>  control API port (default 4811; 0 picks a free port)
  --host <addr>       bind address (default 127.0.0.1; the container image uses 0.0.0.0)
  --seed <text>       seed for the identifiers (default workbench)
  --fixtures <name>   "default" or a path to a fixture JSON file
  --anchor <iso>      the date-time fixture times are relative to (default: the fixture set's anchor)
  --help              this text

Endpoints
  OAuth (Entra-shaped), tenant = any id, for example "mock-tenant":
    GET  /{tenant}/v2.0/.well-known/openid-configuration
    GET  /{tenant}/oauth2/v2.0/authorize   (auto-consents for the login_hint user or the default user; PKCE S256 required)
    POST /{tenant}/oauth2/v2.0/token       (authorization_code, refresh_token)
  Graph: /v1.0/me, /me/mailboxSettings, /me/mailFolders, /me/messages (OData subset), attachments, delta, drafts,
    send, move, delete, /me/events, /me/calendarView, /me/calendarView/delta, /me/calendar/getSchedule, responses.
  Control (separate port), under /_mock:
    GET /health, GET /users, POST /reset {seed, fixtures, anchor}
    POST /users/{user}/messages, DELETE /users/{user}/messages/{id}
    POST /users/{user}/events, PATCH /users/{user}/events/{id}, POST /users/{user}/events/{id}/cancel
    GET|DELETE /sends, GET|DELETE /requests
    POST /faults {kind: throttle|server_error|delta_expired|slow|malformed, method, path, count, retry_after, delay_ms, status}
    GET|DELETE /faults, POST /delta/expire
    POST /oauth/faults {kind: interaction_required|consent_required|invalid_grant|expired_token, user, step, count}
    POST /oauth/config {access_token_lifetime_s, refresh_token_lifetime_s}, POST /oauth/revoke {user}

Client policy for development builds: mail.auth = "mock", mail.graph_base_url = "<graph>/v1.0",
mail.oauth = {authority: "<graph>", tenant_id: "mock-tenant", client_id: "workbench-dev"}.
`;

async function main(): Promise<void> {
  const { values } = parseArgs({
    options: {
      port: { type: 'string', default: '4810' },
      'control-port': { type: 'string', default: '4811' },
      host: { type: 'string', default: '127.0.0.1' },
      seed: { type: 'string' },
      fixtures: { type: 'string' },
      anchor: { type: 'string' },
      help: { type: 'boolean', default: false },
    },
  });
  if (values.help) {
    process.stdout.write(HELP);
    return;
  }
  const mock = new GraphMock({ seed: values.seed, fixtures: values.fixtures, anchor: values.anchor, host: values.host });
  await mock.listen(Number(values.port), Number(values['control-port']));
  process.stdout.write(JSON.stringify({ graph: mock.graphBase, control: mock.controlBase, authority: mock.graphBase, tenant: 'mock-tenant' }) + '\n');
  for (const sig of ['SIGINT', 'SIGTERM'] as const) process.on(sig, () => void mock.close().then(() => process.exit(0)));
}

main().catch((e: unknown) => {
  process.stderr.write(`graph-mock: ${(e as Error).message}\n`);
  process.exit(1);
});
