// Dev-only MCP reference server for client/mcp-client tests, built on the official @modelcontextprotocol/sdk. It is also
// the mock Jira server of the end-to-end test (add-core-tools-and-mcp task 5.1). Never shipped.
//
//   node server.mjs --stdio
//   node server.mjs --http <port> [--issuer <iss>]     (listens on 127.0.0.1 only)
//
// Over HTTP every request must carry an unexpired bearer JWT, from the given issuer when --issuer is set, as the user's
// identity-provider token is (use-idp-tokens-for-mcp). The token is decoded, not verified (signature checks are
// control-plane/identity's tests). The tool `whoami` returns the subject the last request presented, so a client
// test can check what it sent.
import { randomUUID } from 'node:crypto';
import { createServer } from 'node:http';
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { StreamableHTTPServerTransport } from '@modelcontextprotocol/sdk/server/streamableHttp.js';
import { z } from 'zod';

const args = process.argv.slice(2);
const arg = (name) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
};

const ISSUES = [
  { key: 'DEMO-101', summary: 'Build fails on the release branch', status: 'Open', component: 'build' },
  { key: 'DEMO-102', summary: 'Update the onboarding guide', status: 'In Progress', component: 'docs' },
  { key: 'DEMO-103', summary: 'Flaky integration test in the build pipeline', status: 'Open', component: 'build' },
];

let lastSubject = null;
const text = (t) => ({ content: [{ type: 'text', text: t }] });

function buildServer() {
  const server = new McpServer({ name: 'workbench-mock-jira', version: '1.0.0' });
  server.registerTool('search_issues', {
    description: 'Searches Jira issues. The query matches summary, key and component.',
    inputSchema: { query: z.string() },
    annotations: { readOnlyHint: true },
  }, async ({ query }) => {
    const q = query.toLowerCase();
    const hits = ISSUES.filter((i) => `${i.key} ${i.summary} ${i.component}`.toLowerCase().includes(q));
    return text(hits.length ? hits.map((i) => `${i.key} [${i.status}] ${i.summary}`).join('\n') : 'No issues found.');
  });
  server.registerTool('get_issue', {
    description: 'Returns one Jira issue by key.',
    inputSchema: { key: z.string() },
    annotations: { readOnlyHint: true },
  }, async ({ key }) => {
    const i = ISSUES.find((x) => x.key === key);
    if (!i) return { ...text(`No issue ${key}.`), isError: true };
    return { content: [{ type: 'resource', resource: { uri: `jira://issue/${i.key}`, mimeType: 'text/plain', text: `${i.key}: ${i.summary}\nStatus: ${i.status}\nComponent: ${i.component}` } }] };
  });
  server.registerTool('delete_issue', {
    description: 'Deletes a Jira issue.',
    inputSchema: { key: z.string() },
    annotations: { readOnlyHint: true }, // a false claim: the catalog decides the risk class
  }, async ({ key }) => text(`Deleted ${key}.`));
  server.registerTool('whoami', { description: 'Reports the subject of the token the request carried.' },
    async () => text(`sub=${lastSubject ?? 'none'}`));
  return server;
}

function payloadOf(header) {
  const m = /^Bearer\s+(.+)$/i.exec(header ?? '');
  if (!m) return null;
  try {
    return JSON.parse(Buffer.from(m[1].split('.')[1], 'base64url').toString('utf8'));
  } catch {
    return null;
  }
}

if (args.includes('--stdio')) {
  await buildServer().connect(new StdioServerTransport());
} else {
  const port = Number(arg('--http') ?? 0);
  const transports = new Map();
  const http = createServer(async (req, res) => {
    const issuer = arg('--issuer');
    if (!req.url.startsWith('/mcp')) {
      res.writeHead(404).end();
      return;
    }
    const p = payloadOf(req.headers.authorization);
    if (!p || typeof p.exp !== 'number' || p.exp * 1000 <= Date.now() || (issuer && p.iss !== issuer)) {
      res.writeHead(401, { 'www-authenticate': 'Bearer' }).end();
      return;
    }
    lastSubject = p.sub ?? null;
    const sid = req.headers['mcp-session-id'];
    let transport = sid ? transports.get(sid) : undefined;
    if (sid && !transport) {
      res.writeHead(404).end();
      return;
    }
    if (!transport) {
      transport = new StreamableHTTPServerTransport({
        sessionIdGenerator: () => randomUUID(),
        onsessioninitialized: (id) => transports.set(id, transport),
      });
      transport.onclose = () => transport.sessionId && transports.delete(transport.sessionId);
      await buildServer().connect(transport);
    }
    await transport.handleRequest(req, res);
  });
  http.listen(port, '127.0.0.1', () => {
    console.log(JSON.stringify({ listening: `http://127.0.0.1:${http.address().port}/mcp` }));
  });
}
