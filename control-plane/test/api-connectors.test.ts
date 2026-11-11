// API connectors (control-plane/api-connectors; add-api-connectors tasks 2.1-2.3, 3.1-3.5, 4.1-4.6, 5.1-5.5). The API is a
// local fake in the test process; no external host is ever contacted.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer, type Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { decodeJwt } from 'jose';
import { readDocument, parseOpenApi } from '../src/api-connectors/openapi.ts';
import { analyzeDocument, apiDescriptionProblem, descriptionProblem } from '../src/api-connectors/rules.ts';
import { checkBaseUrl, isLinkLocalOrMetadata, validateApiSettings } from '../src/api-connectors/settings.ts';
import { startHarness, type Harness } from './helpers.ts';

const ADMIN = '/api/v1/admin/connectors';
const USER_TOKEN = 'tok-alice-pat-value';

const DOC = {
  openapi: '3.1.0',
  info: { title: 'Orders API', description: 'Internal order service: read orders, create orders and search tickets for the support teams.' },
  paths: {
    '/orders/{id}': {
      get: {
        operationId: 'getOrder',
        description: 'Returns one order by its identifier, including its lines and status.',
        parameters: [
          { name: 'id', in: 'path', required: true, schema: { type: 'string' } },
          { name: 'expand', in: 'query', schema: { type: 'string', enum: ['lines', 'payments'] } },
          { name: 'X-Trace', in: 'header', schema: { type: 'string' } },
        ],
      },
      delete: {
        operationId: 'deleteOrder',
        description: 'Deletes an order that has not been shipped yet. This cannot be undone.',
        parameters: [{ name: 'id', in: 'path', required: true, schema: { type: 'string' } }],
      },
    },
    '/orders': {
      post: {
        operationId: 'createOrder',
        description: 'Creates a new order for a customer from the given lines and returns it.',
        requestBody: { required: true, content: { 'application/json': { schema: { $ref: '#/components/schemas/NewOrder' } } } },
      },
    },
    '/search': {
      post: {
        operationId: 'searchTickets',
        description: 'Searches support tickets by text and returns matching ticket summaries only.',
        requestBody: { content: { 'application/json': { schema: { type: 'object', properties: { text: { type: 'string' } } } } } },
      },
    },
  },
  components: {
    schemas: {
      NewOrder: { type: 'object', required: ['customer'], properties: { customer: { type: 'string' }, lines: { type: 'array', items: { type: 'string' } } } },
    },
  },
};

const docText = (doc: unknown) => JSON.stringify(doc);

interface Seen {
  method: string;
  url: string;
  headers: Record<string, string | string[] | undefined>;
  body: string;
}

/** A local API: records each request it receives and answers a few fixed behaviours. */
async function fakeApi() {
  const seen: Seen[] = [];
  let release: () => void = () => {};
  const gate = new Promise<void>((r) => (release = r));
  const server: Server = createServer(async (req, res) => {
    const chunks: Buffer[] = [];
    for await (const c of req) chunks.push(c as Buffer);
    const body = Buffer.concat(chunks).toString('utf8');
    seen.push({ method: req.method!, url: req.url!, headers: req.headers, body });
    const url = new URL(req.url!, 'http://fake.local');
    const send = (status: number, value: unknown) => {
      res.writeHead(status, { 'content-type': 'application/json' });
      res.end(JSON.stringify(value));
    };
    if (url.pathname === '/api/slow') {
      await gate;
      return send(200, { slow: true });
    }
    if (url.pathname === '/api/big') return send(200, { data: 'x'.repeat(300 * 1024) });
    if (url.pathname === '/api/err') {
      res.writeHead(500, { 'content-type': 'text/plain' });
      return res.end('internal failure '.repeat(300));
    }
    if (url.pathname === '/api/binary') {
      res.writeHead(200, { 'content-type': 'image/png' });
      return res.end(Buffer.from([0x89, 0x50, 0x4e, 0x47]));
    }
    if (url.pathname === '/api/redirect') {
      res.writeHead(302, { location: '/api/orders/1' });
      return res.end();
    }
    if (url.pathname.startsWith('/api/orders/')) {
      if (req.headers.authorization === 'Bearer revoked-value') return send(401, { error: 'expired' });
      if (req.headers.authorization === 'Bearer forbidden-value') return send(403, { error: 'no access for forbidden-value' });
      return send(200, { id: decodeURIComponent(url.pathname.split('/')[3]!), query: url.search });
    }
    if (url.pathname === '/api/orders' && req.method === 'POST') return send(201, JSON.parse(body || 'null'));
    send(404, { error: 'no such path' });
  });
  await new Promise<void>((r) => server.listen(0, '127.0.0.1', r));
  const port = (server.address() as AddressInfo).port;
  return {
    base: `http://127.0.0.1:${port}/api`,
    seen,
    release: () => release(),
    close: () => new Promise<void>((r) => {
      server.closeAllConnections();
      server.close(() => r());
    }),
  };
}

const devOn = (c: { apiConnectors: { allowLoopbackHttp: boolean }; mcpCatalog: { cacheSeconds: number } }) => {
  c.apiConnectors.allowLoopbackHttp = true;
  c.mcpCatalog.cacheSeconds = 0;
};

async function setup(h: Harness) {
  const admin = await h.token('admin1');
  for (const u of ['dev1', 'dev2']) {
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: u, workbench_group: 'developers' });
  }
  return { admin, dev1: await h.token('dev1'), dev2: await h.token('dev2') };
}

/** Uploads a document as text, so that the application's JSON parser does not take it. */
async function upload(h: Harness, admin: string, id: string, text: string) {
  const r = await fetch(`${h.base}${ADMIN}/${id}/document`, {
    method: 'PUT',
    headers: { authorization: `Bearer ${admin}`, 'content-type': 'text/plain' },
    body: text,
  });
  return { status: r.status, body: (await r.json()) as any };
}

async function createConnector(h: Harness, admin: string, fake: { base: string }, extra: Record<string, unknown> = {}) {
  const r = await h.call('POST', ADMIN, admin, {
    id: 'orders-api',
    owner: 'it-orders',
    name: 'Orders',
    kind: 'api',
    base_url: fake.base,
    auth: 'none',
    groups: ['developers'],
    ...extra,
  });
  assert.equal(r.status, 200, JSON.stringify(r.body));
  return r;
}

async function bundleServers(h: Harness, user: string): Promise<any[]> {
  const r = await h.call('GET', '/api/v1/policy/bundle', await h.token(user));
  assert.equal(r.status, 200, JSON.stringify(r.body));
  return ((decodeJwt(r.body.bundle).policy as any)?.mcp?.servers ?? []) as any[];
}

// ---- document, descriptions and usability (tasks 3.1 to 3.4) ------------------------------------------------------------

test('api connectors: Swagger 2.0, external references, bad versions and malformed text are refused', () => {
  const bad = [
    { swagger: '2.0', info: { title: 'x', description: 'y' }, paths: {} },
    { openapi: '2.0', info: { title: 'x' }, paths: {} },
    { ...DOC, paths: { '/a': { get: { description: 'x', responses: { 200: { $ref: 'https://example.test/schemas.json#/a' } } } } } },
    { ...DOC, components: { schemas: { A: { $ref: 'other.yaml#/B' } } } },
  ];
  for (const doc of bad) {
    assert.throws(() => parseOpenApi(readDocument(JSON.stringify(doc), 5 * 1024 * 1024)), (e: any) => e.status === 400, JSON.stringify(doc).slice(0, 80));
  }
  assert.throws(() => readDocument('{ not json', 1000), (e: any) => e.status === 400);
  assert.throws(() => readDocument('x'.repeat(2000), 1000), (e: any) => e.status === 413);
  assert.throws(() => readDocument('- a\n- b\n', 1000), (e: any) => e.status === 400);
  const swagger = parseOpenApi(readDocument(JSON.stringify(DOC), 5 * 1024 * 1024));
  assert.equal(swagger.openapi, '3.1.0');
});

test('api connectors: a YAML alias bomb is refused by the parser limits', () => {
  // Each level holds ten aliases of the level before: the alias limit (50) is passed by the sixth level.
  const lines = ['openapi: 3.1.0', 'info:', '  title: Bomb', '  description: "' + 'x'.repeat(100) + '"', 'paths: {}', 'l0: &l0 ["x","x","x","x","x","x","x","x","x","x"]'];
  for (let i = 1; i <= 6; i++) lines.push(`l${i}: &l${i} [${Array(10).fill(`*l${i - 1}`).join(',')}]`);
  assert.throws(() => readDocument(lines.join('\n'), 5 * 1024 * 1024), (e: any) => e.status === 400);
});

test('api connectors: a fuzzed document never crashes and never yields a partial result', () => {
  let seed = 7;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const alphabet = ['{', '}', '[', ']', ':', ',', '"', '$ref', '#/', 'paths', 'get', 'post', 'openapi', '3.1.0', '\n', '  ', '&a', '*a', '1', 'null'];
  for (let i = 0; i < 300; i++) {
    let text = JSON.stringify(DOC);
    const edits = 1 + Math.floor(rnd() * 6);
    for (let k = 0; k < edits; k++) {
      const at = Math.floor(rnd() * text.length);
      const piece = alphabet[Math.floor(rnd() * alphabet.length)]!;
      text = rnd() < 0.5 ? text.slice(0, at) + piece + text.slice(at) : text.slice(0, at) + text.slice(at + 1);
    }
    try {
      const doc = readDocument(text, 5 * 1024 * 1024);
      const parsed = parseOpenApi(doc);
      analyzeDocument(parsed);
    } catch (err) {
      assert.ok(err && typeof err === 'object' && 'status' in err && (err as any).status >= 400 && (err as any).status < 500, `unexpected error on case ${i}: ${String(err)}`);
    }
  }
});

test('api connectors: description rules at the bounds, placeholders and copied titles (table-driven)', () => {
  const op = (n: number) => 'a'.repeat(n);
  const bounds = { min: 40, max: 1000 };
  const cases: [string, number, boolean][] = [
    ['39 characters', 39, false],
    ['40 characters', 40, true],
    ['1000 characters', 1000, true],
    ['1001 characters', 1001, false],
  ];
  for (const [name, len, ok] of cases) {
    assert.equal(descriptionProblem(op(len), bounds, []) === null, ok, `operation ${name}`);
  }
  const apiCases: [number, boolean][] = [
    [79, false],
    [80, true],
    [2000, true],
    [2001, false],
  ];
  for (const [len, ok] of apiCases) {
    assert.equal(apiDescriptionProblem(op(len), 'Orders API') === null, ok, `API ${len}`);
  }
  assert.match(descriptionProblem('TODO', bounds, []) ?? '', /placeholder/);
  assert.match(descriptionProblem('n/a', bounds, []) ?? '', /placeholder/);
  assert.match(apiDescriptionProblem('Orders API', 'Orders API') ?? '', /its own description/);
  const repeated = 'GET /orders/{id}/lines/for/the/repeat/check';
  assert.match(descriptionProblem(repeated, bounds, [{ text: repeated, label: 'method and path' }]) ?? '', /repeats the method and path/);
  // Whitespace is collapsed before counting.
  assert.equal(descriptionProblem(`  ${op(20)}   ${op(20)}  `, bounds, []), null);
});

test('api connectors: operation usability reasons, derived names, duplicates and parameter rules', () => {
  const doc = {
    openapi: '3.0.3',
    info: { title: 'Mixed API', description: 'A deliberately mixed test document covering the usability reasons of every operation.' },
    paths: {
      '/items/{id}': {
        parameters: [{ name: 'id', in: 'path', required: true, schema: { type: 'string' } }],
        get: { description: 'Returns one item and needs no operationId, so its name is derived.', parameters: [{ name: 'body', in: 'query', schema: { type: 'string' } }] },
        head: { description: 'Head requests are not an operation kind that is supported here.' },
      },
      '/forms': {
        post: {
          operationId: 'submitForm',
          description: 'Submits a form, which is not a JSON request body and so is not supported.',
          requestBody: { content: { 'application/x-www-form-urlencoded': { schema: { type: 'object' } } } },
        },
      },
      '/dup/a': { get: { operationId: 'same', description: 'The first of two operations that share one operationId in this document.' } },
      '/dup/b': { get: { operationId: 'same', description: 'The second of two operations that share one operationId in this document.' } },
      '/ok/{x}': { get: { operationId: 'okOne', description: 'A usable operation whose header parameter is ignored and never sent.', parameters: [{ name: 'x', in: 'path', required: true, schema: { type: 'string' } }, { name: 'Accept-Language', in: 'header', schema: { type: 'string' } }] } },
      '/deep': {
        get: { operationId: 'deepOne', description: 'An operation whose schema refers to itself, so the reference chain is too deep.', responses: {} },
      },
    },
    components: { schemas: { Loop: { type: 'object', properties: { next: { $ref: '#/components/schemas/Loop' } } } } },
  } as any;
  doc.paths['/deep'].get.requestBody = { content: { 'application/json': { schema: { $ref: '#/components/schemas/Loop' } } } };
  const { operations } = analyzeDocument(parseOpenApi(doc));
  const by = (k: string) => operations.find((o) => o.key === k)!;
  assert.equal(by('GET /items/{id}').tool, 'get__items__id_', 'name derived from method and path');
  assert.equal(by('GET /items/{id}').usable, false);
  assert.match(by('GET /items/{id}').reason!, /parameter named body/);
  assert.match(by('HEAD /items/{id}').reason ?? '', /not supported/);
  assert.match(by('POST /forms').reason ?? '', /only JSON request bodies are supported/);
  assert.equal(by('GET /dup/a').usable, false);
  assert.match(by('GET /dup/a').reason ?? '', /more than one operation/);
  assert.equal(by('GET /ok/{x}').usable, true);
  assert.equal(by('GET /ok/{x}').inputSchema!.properties && 'Accept-Language' in (by('GET /ok/{x}').inputSchema!.properties as object), false, 'header parameters are not in the schema');
  assert.match(by('GET /deep').reason ?? '', /too deep/);
});

test('api connectors: the tool schema maps parameters and the body, with references resolved inside the document', () => {
  const { operations } = analyzeDocument(parseOpenApi(DOC as any));
  const create = operations.find((o) => o.key === 'POST /orders')!;
  assert.equal(create.usable, true);
  assert.deepEqual(create.inputSchema!.required, ['body']);
  const body = (create.inputSchema!.properties as any).body;
  assert.equal(body.properties.customer.type, 'string', 'the reference to NewOrder is inlined');
  const get = operations.find((o) => o.key === 'GET /orders/{id}')!;
  assert.deepEqual(get.inputSchema!.required, ['id']);
  assert.deepEqual(get.callSpec, { method: 'get', path: '/orders/{id}', pathParams: ['id'], queryParams: ['expand'], body: false });
  assert.ok(get.schemaBytes > 0 && get.schemaBytes < 16 * 1024);
});

// ---- settings, secrets and address rules (tasks 2.1, 4.1, 4.2) -----------------------------------------------------------

test('api connectors: settings refuse a key in the query string, HTTP outside loopback, link-local and copied headers', () => {
  const ok = { base_url: 'https://orders.internal.example.test/api', auth: 'api-key', key_header: 'X-Api-Key' };
  assert.equal(validateApiSettings(ok, false).auth, 'api-key');
  assert.throws(() => validateApiSettings({ ...ok, key_in: 'query' }, false), (e: any) => e.extra.field === 'key_in');
  assert.throws(() => validateApiSettings({ ...ok, base_url: 'http://orders.internal.example.test/api' }, false), (e: any) => e.extra.field === 'base_url');
  assert.equal(validateApiSettings({ ...ok, base_url: 'http://127.0.0.1:9/api' }, true).base_url, 'http://127.0.0.1:9/api');
  assert.throws(() => validateApiSettings({ ...ok, base_url: 'http://127.0.0.1:9/api' }, false), (e: any) => e.extra.field === 'base_url');
  assert.throws(() => validateApiSettings({ ...ok, base_url: 'https://169.254.169.254/latest' }, true), (e: any) => e.extra.field === 'base_url');
  assert.throws(() => validateApiSettings({ ...ok, key_header: 'Host' }, false), (e: any) => e.extra.field === 'key_header');
  assert.throws(() => validateApiSettings({ ...ok, key_header: 'bad header' }, false), (e: any) => e.extra.field === 'key_header');
  assert.throws(() => validateApiSettings({ base_url: 'https://orders.example.test', auth: 'api-key' }, false), (e: any) => e.extra.field === 'key_header');
  assert.throws(() => validateApiSettings({ base_url: 'https://orders.example.test', auth: 'oauth' }, false), (e: any) => e.extra.field === 'auth');
  assert.equal(isLinkLocalOrMetadata('fe80::1'), true);
  assert.equal(isLinkLocalOrMetadata('10.0.0.5'), false, 'private addresses are allowed (D10)');
  assert.equal(checkBaseUrl('https://10.0.0.5/api', false), 'https://10.0.0.5/api');
});

test('api connectors: user-pat settings need a header, help text of 20 to 500 characters and an HTTPS help link', () => {
  const pat = { base_url: 'https://jira.internal.example.test/rest', auth: 'user-pat', key_header: 'Authorization', key_scheme: 'Bearer' };
  assert.throws(() => validateApiSettings(pat, false), (e: any) => e.extra.field === 'help_text');
  assert.throws(() => validateApiSettings({ ...pat, help_text: 'too short' }, false), (e: any) => e.extra.field === 'help_text');
  assert.throws(() => validateApiSettings({ ...pat, help_text: 'x'.repeat(501) }, false), (e: any) => e.extra.field === 'help_text');
  assert.throws(() => validateApiSettings({ ...pat, help_text: 'Create a token under Profile, Personal access tokens.', help_link: 'http://jira.example.test' }, false), (e: any) => e.extra.field === 'help_link');
  assert.throws(() => validateApiSettings({ ...pat, key_scheme: 'Bearer 1' , help_text: 'Create a token under Profile, Personal access tokens.' }, false), (e: any) => e.extra.field === 'key_scheme');
  const s = validateApiSettings({ ...pat, help_text: '  Create a token under   Profile, Personal access tokens.  ', help_link: 'https://jira.internal.example.test/tokens' }, false);
  assert.equal(s.help_text, 'Create a token under Profile, Personal access tokens.');
  assert.equal(s.key_scheme, 'Bearer');
});

// ---- the admin API: upload, report, selection and risk (tasks 3.5, 4.1, 7.1 API side) -------------------------------------

test('api connectors: upload reports every operation, selection refuses an unusable one, and a re-upload flags and retires', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    await createConnector(h, admin, fake);
    const doc = { ...DOC, paths: { ...DOC.paths, '/bad': { get: { operationId: 'badOne', description: 'TODO' } } } };
    const up = await upload(h, admin, 'orders-api', docText(doc));
    assert.equal(up.status, 200, JSON.stringify(up.body));
    // Default risk classes by method (Tool Risk Classes): GET read, POST and PUT and PATCH external, DELETE destructive.
    const riskOf = (key: string) => up.body.operations.find((o: any) => o.key === key).risk;
    assert.equal(riskOf('GET /orders/{id}'), 'read');
    assert.equal(riskOf('POST /orders'), 'external');
    assert.equal(riskOf('DELETE /orders/{id}'), 'destructive');
    const bad = up.body.operations.find((o: any) => o.key === 'GET /bad');
    assert.equal(bad.usable, false);
    assert.match(bad.reason, /placeholder/);
    assert.equal(up.body.operations.find((o: any) => o.key === 'GET /orders/{id}').usable, true);
    assert.equal(up.body.counts.selected, 0, 'nothing is selected by default');

    const refused = await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /bad', selected: true }] });
    assert.equal(refused.status, 400);
    assert.match(refused.body.error.message, /cannot be selected: its description is a placeholder/);
    assert.equal(refused.body.error.field, 'key');

    const risky = await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'POST /search', selected: true, risk: 'read' }] });
    assert.equal(risky.status, 400, 'a POST lowered to read needs a reason');
    assert.equal(risky.body.error.field, 'risk_reason');

    const saved = await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, {
      operations: [
        { key: 'GET /orders/{id}', selected: true },
        { key: 'POST /search', selected: true, risk: 'read', risk_reason: 'search only, no change to data' },
      ],
    });
    assert.equal(saved.status, 200, JSON.stringify(saved.body));
    assert.equal(saved.body.counts.selected, 2);

    // A new document adds an operation (flagged, not offered) and removes searchTickets (gone, its calls refused).
    const next = { ...DOC, paths: { '/orders/{id}': DOC.paths['/orders/{id}'], '/orders': DOC.paths['/orders'], '/refunds': { post: { operationId: 'refund', description: 'Refunds the payment of one order in full, to the original card.' } } } };
    const re = await upload(h, admin, 'orders-api', docText(next));
    assert.equal(re.status, 200);
    assert.equal(re.body.operations.find((o: any) => o.key === 'POST /refunds').status, 'new');
    assert.equal(re.body.operations.find((o: any) => o.key === 'POST /search').status, 'gone');
    const offered = await bundleServers(h, 'dev1');
    const tools = (offered.find((s) => s.id === 'orders-api')?.operations ?? []).map((o: any) => o.name);
    assert.deepEqual(tools, ['getOrder'], 'only the still-selected, still-present operation is offered');

    const call = await h.call('POST', '/api/v1/connectors/orders-api/operations/searchTickets/call', dev1, { arguments: { body: { text: 'x' } } });
    assert.equal(call.status, 403, 'a gone operation is refused');
    assert.equal(call.body.error.code, 'NOT_OFFERED');
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: a non-GET selected as read keeps its reason in the audit trail, and the API description is read-only', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin } = await setup(h);
    await createConnector(h, admin, fake);
    await upload(h, admin, 'orders-api', docText(DOC));
    const ok = await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, {
      operations: [{ key: 'POST /search', selected: true, risk: 'read', risk_reason: 'search only, no change to data' }],
    });
    assert.equal(ok.status, 200);
    const audit = h.cp.db.prepare("SELECT details FROM audit WHERE action = 'api.operations.save'").all() as { details: string }[];
    assert.ok(audit.some((r) => /search only, no change to data/.test(r.details)), 'the audit trail records the risk reason');
    // A description override is not accepted: the settings body has no field for it, and the stored text is the document's.
    const settings = await h.call('PUT', `${ADMIN}/orders-api/settings`, admin, { description: 'My own text', base_url: fake.base, auth: 'none' });
    assert.equal(settings.status, 200);
    const detail = await h.call('GET', `${ADMIN}/orders-api`, admin);
    assert.equal(detail.body.api.operations.find((o: any) => o.key === 'GET /orders/{id}').description, DOC.paths['/orders/{id}'].get.description);
  } finally {
    await h.close();
    await fake.close();
  }
});

// ---- secrets, the bundle and the authorization of calls (tasks 2.2, 2.3, 4.2, 5.1) ----------------------------------------

test('api connectors: an api-key is write-only, encrypted at rest, and absent from the bundle and the detail view', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    const secret = 'sk-live-test-key-9f8e7d6c';
    const created = await createConnector(h, admin, fake, { auth: 'api-key', key_header: 'X-Api-Key', api_key: secret });
    assert.equal(created.body.api.key.set, true);
    assert.equal(created.body.api.key.hint, '7d6c');
    assert.doesNotMatch(JSON.stringify(created.body), /sk-live-test-key/);
    await upload(h, admin, 'orders-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });
    assert.doesNotMatch(JSON.stringify(await bundleServers(h, 'dev1')), /sk-live|fake\.local|127\.0\.0\.1/, 'no key and no base URL in the bundle');
    const rows = h.cp.db.prepare('SELECT key_enc FROM api_connector_secrets').all() as { key_enc: string }[];
    assert.equal(rows.length, 1);
    assert.doesNotMatch(rows[0]!.key_enc, /sk-live/);
    const got = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '42' } });
    assert.equal(got.status, 200, JSON.stringify(got.body));
    assert.equal(fake.seen.at(-1)!.headers['x-api-key'], secret, 'the key is sent in its header');
    // Replacing the key replaces it; a settings change to another mode removes it.
    await h.call('PUT', `${ADMIN}/orders-api/settings`, admin, { base_url: fake.base, auth: 'api-key', key_header: 'X-Api-Key', api_key: 'replacement-key-123456' });
    assert.equal((h.cp.db.prepare('SELECT COUNT(*) AS n FROM api_connector_secrets').get() as { n: number }).n, 1);
    await h.call('PUT', `${ADMIN}/orders-api/settings`, admin, { base_url: fake.base, auth: 'none' });
    assert.equal((h.cp.db.prepare('SELECT COUNT(*) AS n FROM api_connector_secrets').get() as { n: number }).n, 0);
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: a non-member cannot call, an unselected operation is refused, invalid arguments send nothing', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1, dev2 } = await setup(h);
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'outsider', workbench_group: 'sales' });
    await createConnector(h, admin, fake);
    await upload(h, admin, 'orders-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });
    const before = fake.seen.length;

    const outsider = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', await h.token('outsider'), { arguments: { id: '1' } });
    assert.equal(outsider.status, 403);
    assert.match(outsider.body.error.message, /not in your catalog/);

    const unselected = await h.call('POST', '/api/v1/connectors/orders-api/operations/createOrder/call', dev1, { arguments: { body: { customer: 'a' } } });
    assert.equal(unselected.status, 403);
    assert.equal(unselected.body.error.code, 'NOT_OFFERED');

    const missing = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { expand: 'lines' } });
    assert.equal(missing.status, 400);
    assert.equal(missing.body.error.field, 'id');
    const unknown = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '1', nope: 2 } });
    assert.equal(unknown.status, 400);
    const enumBad = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '1', expand: 'secrets' } });
    assert.equal(enumBad.status, 400);
    assert.equal(fake.seen.length, before, 'no request reached the API');

    const ok = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev2, { arguments: { id: '7', expand: 'lines' } });
    assert.equal(ok.status, 200, JSON.stringify(ok.body));
    assert.equal(ok.body.body.id, '7');
    assert.equal(fake.seen.at(-1)!.url, '/api/orders/7?expand=lines');
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: upstream request rules, path refusals and response limits, against the fake API', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    await createConnector(h, admin, fake);
    await upload(h, admin, 'orders-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }, { key: 'POST /orders', selected: true }] });
    const get = (id: string) => h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id } });

    const dots = await get('..');
    assert.equal(dots.status, 400);
    const other = await get('https://evil.example.test/x');
    assert.equal(other.status, 403);
    assert.match(other.body.error.message, /another address/);
    const encoded = await get('a/b c');
    assert.equal(encoded.status, 200);
    assert.equal(fake.seen.at(-1)!.url, '/api/orders/a%2Fb%20c', 'path values are percent-encoded');

    const created = await h.call('POST', '/api/v1/connectors/orders-api/operations/createOrder/call', dev1, { arguments: { body: { customer: 'Acme', lines: ['x'] } } });
    assert.equal(created.status, 200, JSON.stringify(created.body));
    assert.equal(created.body.status, 201);
    assert.deepEqual(JSON.parse(fake.seen.at(-1)!.body), { customer: 'Acme', lines: ['x'] });
    const noBody = await h.call('POST', '/api/v1/connectors/orders-api/operations/createOrder/call', dev1, { arguments: {} });
    assert.equal(noBody.status, 400, 'a required body is checked before any request');
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: a redirect is an error, a large response is cut, a 500 carries its text, and binary is refused', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    await createConnector(h, admin, fake);
    // The fake's paths are reached through the operation's own path, so a second document maps them.
    const extra = {
      openapi: '3.1.0',
      info: { title: 'Probe API', description: 'A probe document that maps the fake responses for the response limit tests here.' },
      paths: {
        '/big': { get: { operationId: 'probeBig', description: 'Returns a very large body so that the response limit cuts it short.' } },
        '/err': { get: { operationId: 'probeErr', description: 'Returns an internal error with a long text body for the error text limit.' } },
        '/binary': { get: { operationId: 'probeBinary', description: 'Returns an image, which is a binary response that is refused.' } },
        '/redirect': { get: { operationId: 'probeRedirect', description: 'Answers with a redirect, which the control plane never follows.' } },
      },
    };
    await upload(h, admin, 'orders-api', docText(extra));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, {
      operations: ['GET /big', 'GET /err', 'GET /binary', 'GET /redirect'].map((key) => ({ key, selected: true })),
    });
    const probe = (op: string) => h.call('POST', `/api/v1/connectors/orders-api/operations/${op}/call`, dev1, { arguments: {} });
    const big = await probe('probeBig');
    assert.equal(big.status, 200, JSON.stringify(big.body));
    assert.equal(big.body.truncated, true);
    assert.equal(typeof big.body.body, 'string', 'a cut JSON body is returned as text');
    assert.ok(Buffer.byteLength(big.body.body, 'utf8') <= 256 * 1024);
    const err = await probe('probeErr');
    assert.equal(err.status, 502);
    assert.equal(err.body.error.code, 'UPSTREAM_ERROR');
    assert.equal(err.body.error.status, 500);
    assert.ok(err.body.error.text.length <= 2048);
    const bin = await probe('probeBinary');
    assert.equal(bin.status, 415);
    assert.match(bin.body.error.message, /Binary responses are not supported/);
    const red = await probe('probeRedirect');
    assert.equal(red.status, 502);
    assert.match(red.body.error.message, /redirect \(302\)/);
    assert.equal(fake.seen.filter((s) => s.url === '/api/orders/1').length, 0, 'the redirect target is not requested');
    // No response body is stored.
    const stored = h.cp.db.prepare("SELECT name FROM sqlite_master WHERE type = 'table' AND name LIKE 'api_%'").all() as { name: string }[];
    for (const t of stored) {
      const cols = (h.cp.db.prepare(`PRAGMA table_info(${t.name})`).all() as { name: string }[]).map((c) => c.name);
      assert.ok(!cols.some((c) => /response|body/.test(c)), `${t.name} has no response column`);
    }
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: the ninth concurrent call to one connector is refused at once as busy', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    await createConnector(h, admin, fake);
    const slow = {
      openapi: '3.1.0',
      info: { title: 'Slow API', description: 'A document whose one operation waits on the fake API so that calls overlap in time.' },
      paths: { '/slow': { get: { operationId: 'slowOne', description: 'Waits for the fake API to release it, so that several calls run at once.' } } },
    };
    await upload(h, admin, 'orders-api', docText(slow));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /slow', selected: true }] });
    const calls = Array.from({ length: 8 }, () => h.call('POST', '/api/v1/connectors/orders-api/operations/slowOne/call', dev1, { arguments: {} }));
    await new Promise((r) => setTimeout(r, 200));
    const ninth = await h.call('POST', '/api/v1/connectors/orders-api/operations/slowOne/call', dev1, { arguments: {} });
    assert.equal(ninth.status, 429);
    assert.equal(ninth.body.error.code, 'BUSY');
    fake.release();
    const done = await Promise.all(calls);
    assert.ok(done.every((r) => r.status === 200));
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: each call is audited with names only, and no argument value is recorded', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    await createConnector(h, admin, fake);
    await upload(h, admin, 'orders-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });
    const ok = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: 'secret-order-77', expand: 'payments' } });
    assert.equal(ok.status, 200);
    const rows = h.cp.db.prepare("SELECT outcome, details, user_name FROM audit WHERE action = 'api.call'").all() as { outcome: string; details: string; user_name: string }[];
    assert.equal(rows.length, 1);
    assert.equal(rows[0]!.outcome, 'ok');
    assert.equal(rows[0]!.user_name, 'dev1');
    const details = JSON.parse(rows[0]!.details);
    assert.equal(details.connector, 'orders-api');
    assert.equal(details.operation, 'getOrder');
    assert.equal(details.status, 200);
    assert.deepEqual(details.arguments.sort(), ['expand', 'id']);
    assert.equal(typeof details.duration_ms, 'number');
    assert.equal(typeof details.response_bytes, 'number');
    assert.doesNotMatch(rows[0]!.details, /secret-order-77|payments/);
  } finally {
    await h.close();
    await fake.close();
  }
});

// ---- personal access tokens (tasks 4.3 to 4.6) -----------------------------------------------------------------------

const PAT_CONNECTOR = {
  auth: 'user-pat',
  key_header: 'Authorization',
  key_scheme: 'Bearer',
  help_text: 'Create a token under your profile, in Personal access tokens.',
  help_link: 'https://tokens.internal.example.test/new',
};

test('api connectors: a personal access token is set per user, never returned, and gates the connector tools', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1, dev2 } = await setup(h);
    await createConnector(h, admin, fake, PAT_CONNECTOR);
    await upload(h, admin, 'orders-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });

    // Without a token: the bundle has the connector with needs-token and no operations; the listing says so; a call is refused.
    const bundle0 = (await bundleServers(h, 'dev1')).find((s) => s.id === 'orders-api');
    assert.equal(bundle0.state, 'needs-token');
    assert.deepEqual(bundle0.operations, []);
    const list0 = await h.call('GET', '/api/v1/connectors', dev1);
    const item0 = list0.body.connectors.find((c: any) => c.id === 'orders-api');
    assert.equal(item0.state, 'needs-token');
    assert.equal(item0.offered_tools, 0);
    assert.match(item0.help_text, /Personal access tokens/);
    assert.doesNotMatch(JSON.stringify(list0.body), /127\.0\.0\.1|tok-alice/);
    const noTok = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '1' } });
    assert.equal(noTok.status, 403);
    assert.equal(noTok.body.error.code, 'TOKEN_MISSING');
    assert.equal(fake.seen.length, 0, 'nothing was sent without a token');

    // Set: the status says set with a time, and nothing else.
    const set = await h.call('PUT', '/api/v1/connectors/orders-api/token', dev1, { token: USER_TOKEN });
    assert.equal(set.status, 200, JSON.stringify(set.body));
    assert.equal(set.body.state, 'ready');
    assert.equal(set.body.token.set, true);
    assert.doesNotMatch(JSON.stringify(set.body), new RegExp(USER_TOKEN));
    const stored = h.cp.db.prepare('SELECT token_enc FROM api_connector_tokens').get() as { token_enc: string };
    assert.doesNotMatch(stored.token_enc, new RegExp(USER_TOKEN), 'stored encrypted');
    const bundle1 = (await bundleServers(h, 'dev1')).find((s) => s.id === 'orders-api');
    assert.equal(bundle1.state, 'ready');
    assert.deepEqual(bundle1.operations.map((o: any) => o.name), ['getOrder']);

    // The call sends the token in the configured header with the scheme, to the base URL only.
    const ok = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '5' } });
    assert.equal(ok.status, 200, JSON.stringify(ok.body));
    assert.equal(fake.seen.at(-1)!.headers.authorization, `Bearer ${USER_TOKEN}`);

    // Another user has no token and sees none of Alice's.
    const list2 = await h.call('GET', '/api/v1/connectors', dev2);
    assert.equal(list2.body.connectors.find((c: any) => c.id === 'orders-api').state, 'needs-token');
    assert.doesNotMatch(JSON.stringify(list2.body), /set_at/);
    const bundle2 = (await bundleServers(h, 'dev2')).find((s) => s.id === 'orders-api');
    assert.deepEqual(bundle2.operations, []);

    // The audit trail holds no token text.
    const audit = h.cp.db.prepare('SELECT details, reason FROM audit').all() as { details: string; reason: string | null }[];
    assert.ok(audit.every((r) => !String(r.details).includes(USER_TOKEN) && !String(r.reason ?? '').includes(USER_TOKEN)));
    assert.ok(audit.length > 0);

    // Removal: the status returns to needs-token.
    const del = await h.call('DELETE', '/api/v1/connectors/orders-api/token', dev1);
    assert.equal(del.body.state, 'needs-token');
    assert.equal((h.cp.db.prepare('SELECT COUNT(*) AS n FROM api_connector_tokens').get() as { n: number }).n, 0);
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: an upstream 401 rejects the token, withholds the tools until it is replaced, and records no token', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1, dev2 } = await setup(h);
    await createConnector(h, admin, fake, PAT_CONNECTOR);
    await upload(h, admin, 'orders-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });
    await h.call('PUT', '/api/v1/connectors/orders-api/token', dev1, { token: 'revoked-value' });
    await h.call('PUT', '/api/v1/connectors/orders-api/token', dev2, { token: USER_TOKEN });
    const dev2Before = (await bundleServers(h, 'dev2')).find((s) => s.id === 'orders-api').operations.length;
    assert.equal(dev2Before, 1);

    const rejected = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '3' } });
    assert.equal(rejected.status, 403);
    assert.equal(rejected.body.error.code, 'TOKEN_REJECTED');
    assert.match(rejected.body.error.message, /Token rejected, replace it\./);

    const list = await h.call('GET', '/api/v1/connectors', dev1);
    const item = list.body.connectors.find((c: any) => c.id === 'orders-api');
    assert.equal(item.state, 'token-rejected');
    assert.equal(item.offered_tools, 0);
    const bundle = (await bundleServers(h, 'dev1')).find((s) => s.id === 'orders-api');
    assert.equal(bundle.state, 'token-rejected');
    assert.deepEqual(bundle.operations, []);
    const again = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '3' } });
    assert.equal(again.body.error.code, 'TOKEN_REJECTED', 'refused without asking the API again');
    assert.equal(fake.seen.filter((s) => s.headers.authorization === 'Bearer revoked-value').length, 1);

    const audit = h.cp.db.prepare("SELECT details FROM audit WHERE action = 'api.token.rejected'").all() as { details: string }[];
    assert.equal(audit.length, 1);
    assert.doesNotMatch(audit[0]!.details, /revoked-value/);
    const other = (await bundleServers(h, 'dev2')).find((s) => s.id === 'orders-api');
    assert.equal(other.operations.length, dev2Before, 'the second user keeps the tools');

    // Replacing the token restores the tools.
    await h.call('PUT', '/api/v1/connectors/orders-api/token', dev1, { token: USER_TOKEN });
    const restored = (await bundleServers(h, 'dev1')).find((s) => s.id === 'orders-api');
    assert.deepEqual(restored.operations.map((o: any) => o.name), ['getOrder']);
    const ok = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '3' } });
    assert.equal(ok.status, 200);
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: an upstream 403 is a refused operation, returns FORBIDDEN with sanitized text, and keeps the token ready', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    await createConnector(h, admin, fake, PAT_CONNECTOR);
    await upload(h, admin, 'orders-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });
    await h.call('PUT', '/api/v1/connectors/orders-api/token', dev1, { token: 'forbidden-value' });

    const refused = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '4' } });
    assert.equal(refused.status, 403);
    assert.equal(refused.body.error.code, 'FORBIDDEN');
    assert.equal(refused.body.error.status, 403, 'the upstream status is returned');
    assert.doesNotMatch(JSON.stringify(refused.body), /forbidden-value/, 'the token is removed from the upstream text');

    const list = await h.call('GET', '/api/v1/connectors', dev1);
    assert.equal(list.body.connectors.find((c: any) => c.id === 'orders-api').state, 'ready', 'a 403 does not reject the token');
    const bundle = (await bundleServers(h, 'dev1')).find((s) => s.id === 'orders-api');
    assert.deepEqual(bundle.operations.map((o: any) => o.name), ['getOrder'], 'the operation stays in the bundle');
    assert.equal((h.cp.db.prepare("SELECT COUNT(*) AS n FROM audit WHERE action = 'api.token.rejected'").get() as { n: number }).n, 0);
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: the user-token path sends the IdP token only to the base URL; none and api-key send no bearer', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    const idp = dev1;
    const callPath = (id: string) => `/api/v1/connectors/${id}/operations/getOrder/call`;

    await createConnector(h, admin, fake, { id: 'idp-api', auth: 'user-token' });
    await upload(h, admin, 'idp-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/idp-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });
    const idpCall = await h.call('POST', callPath('idp-api'), dev1, { arguments: { id: '5' } });
    assert.equal(idpCall.status, 200, JSON.stringify(idpCall.body));
    assert.equal(fake.seen.at(-1)!.headers.authorization, `Bearer ${idp}`, 'the IdP token reaches the connector base URL');

    await createConnector(h, admin, fake, { id: 'open-api', auth: 'none' });
    await upload(h, admin, 'open-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/open-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });
    const openCall = await h.call('POST', callPath('open-api'), dev1, { arguments: { id: '6' } });
    assert.equal(openCall.status, 200, JSON.stringify(openCall.body));
    assert.equal(fake.seen.at(-1)!.headers.authorization, undefined, 'auth none sends no bearer');

    const keyValue = 'key-value-for-path-test-0001';
    await createConnector(h, admin, fake, { id: 'key-api', auth: 'api-key', key_header: 'X-Api-Key', api_key: keyValue });
    await upload(h, admin, 'key-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/key-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });
    const keyCall = await h.call('POST', callPath('key-api'), dev1, { arguments: { id: '7' } });
    assert.equal(keyCall.status, 200, JSON.stringify(keyCall.body));
    assert.equal(fake.seen.at(-1)!.headers.authorization, undefined, 'auth api-key sends no bearer');
    assert.equal(fake.seen.at(-1)!.headers['x-api-key'], keyValue);
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: removing a connector or revoking a user deletes the personal access tokens', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1, dev2 } = await setup(h);
    await createConnector(h, admin, fake, PAT_CONNECTOR);
    await h.call('PUT', '/api/v1/connectors/orders-api/token', dev1, { token: USER_TOKEN });
    await h.call('PUT', '/api/v1/connectors/orders-api/token', dev2, { token: 'second-user-token-value' });
    const removed = await h.call('DELETE', `${ADMIN}/orders-api`, admin);
    assert.equal(removed.status, 200, JSON.stringify(removed.body));
    assert.equal((h.cp.db.prepare('SELECT COUNT(*) AS n FROM api_connector_tokens').get() as { n: number }).n, 0);
    const audit = h.cp.db.prepare("SELECT details FROM audit WHERE action = 'api.tokens.removed'").all() as { details: string }[];
    assert.equal(JSON.parse(audit[0]!.details).tokens_removed, 2);

    // A second connector: a user's revocation deletes that user's tokens only.
    await createConnector(h, admin, fake, { ...PAT_CONNECTOR, id: 'orders-two' });
    await h.call('PUT', '/api/v1/connectors/orders-two/token', dev1, { token: USER_TOKEN });
    await h.call('PUT', '/api/v1/connectors/orders-two/token', dev2, { token: 'second-user-token-value' });
    const rev = await h.call('POST', '/api/v1/admin/fleet/users/revoke', admin, { user: 'dev1', reason: 'left the company' });
    assert.equal(rev.status, 200, JSON.stringify(rev.body));
    const left = h.cp.db.prepare('SELECT user_name FROM api_connector_tokens').all() as { user_name: string }[];
    assert.deepEqual(left.map((r) => r.user_name), ['dev2']);
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: a personal access token is refused on a connector that does not use one, and for an unknown or empty value', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    await createConnector(h, admin, fake);
    const wrong = await h.call('PUT', '/api/v1/connectors/orders-api/token', dev1, { token: USER_TOKEN });
    assert.equal(wrong.status, 400);
    const none = await h.call('PUT', '/api/v1/connectors/nope/token', dev1, { token: USER_TOKEN });
    assert.equal(none.status, 404);
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'outsider', workbench_group: 'sales' });
    const outsider = await h.call('PUT', '/api/v1/connectors/orders-api/token', await h.token('outsider'), { token: USER_TOKEN });
    assert.equal(outsider.status, 404, 'a connector outside the user\'s Groups is not visible to set a token on');
    // The settings of a user-pat connector need help text (task 7.3, API side).
    await h.call('DELETE', `${ADMIN}/orders-api`, admin);
    const { help_text: _unused, ...withoutHelp } = PAT_CONNECTOR;
    const noHelp = await h.call('POST', ADMIN, admin, { id: 'pat-api', owner: 'it', name: 'Jira', kind: 'api', base_url: fake.base, groups: ['developers'], ...withoutHelp });
    assert.equal(noHelp.status, 400, 'a user-pat connector without help text is refused');
    assert.equal(noHelp.body.error.field, 'help_text');
    const withHelp = await createConnector(h, admin, fake, { ...PAT_CONNECTOR, id: 'pat-api' });
    assert.equal(withHelp.body.auth, 'user-pat');
    const bad = await h.call('PUT', `${ADMIN}/pat-api/settings`, admin, { ...PAT_CONNECTOR, help_text: 'short' });
    assert.equal(bad.status, 400);
    assert.equal(bad.body.error.field, 'help_text');
    assert.equal((await h.call('GET', `${ADMIN}/pat-api`, admin)).body.auth, 'user-pat', 'a refused save keeps the previous values');
    const empty = await h.call('PUT', '/api/v1/connectors/pat-api/token', dev1, { token: '   ' });
    assert.equal(empty.status, 400);
  } finally {
    await h.close();
    await fake.close();
  }
});

// ---- catalog kind, group budget and listing (tasks 2.1 to 2.3, 4.6) -------------------------------------------------

test('api connectors: an api entry is catalog-versioned and audited, and the schema budget refuses an over-full Group', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin } = await setup(h);
    await createConnector(h, admin, fake);
    const versions = await h.call('GET', '/api/v1/admin/mcp/servers/orders-api/versions', admin);
    assert.equal(versions.body[0].action, 'mcp.server.create');
    await upload(h, admin, 'orders-api', docText(DOC));
    const v2 = await h.call('GET', '/api/v1/admin/mcp/servers/orders-api/versions', admin);
    assert.equal(v2.body[0].action, 'api.document.upload');
    assert.doesNotMatch(JSON.stringify(v2.body), /key_enc/);

    // Forty-two operations with about 12 KB of schema each: selecting all of them is more than one Group can hold.
    const props = (n: number) => Object.fromEntries(Array.from({ length: n }, (_, i) => [`p${String(i).padStart(3, '0')}`, { type: 'string' }]));
    const big = {
      openapi: '3.1.0',
      info: { title: 'Wide API', description: 'A wide document whose operations carry large input schemas to test the group budget.' },
      paths: Object.fromEntries(
        Array.from({ length: 42 }, (_, i) => [
          `/wide${i}`,
          { get: { operationId: `wide${i}`, description: `Wide operation number ${i} with a large query schema for the budget test.`, parameters: Object.entries(props(500)).map(([name, schema]) => ({ name, in: 'query', schema })) } },
        ]),
      ),
    };
    await h.call('POST', ADMIN, admin, { id: 'wide-api', owner: 'it', name: 'Wide', kind: 'api', base_url: fake.base, auth: 'none', groups: ['developers'] });
    const up = await upload(h, admin, 'wide-api', docText(big));
    assert.equal(up.status, 200, JSON.stringify(up.body).slice(0, 300));
    const all = up.body.operations.map((o: any) => ({ key: o.key, selected: true }));
    const over = await h.call('PUT', `${ADMIN}/wide-api/operations`, admin, { operations: all });
    assert.equal(over.status, 400, 'selecting all 42 is refused');
    assert.match(over.body.error.message, /limit is 524288/);
    const slice = up.body.operations.slice(0, 30).map((o: any) => ({ key: o.key, selected: true }));
    const fits = await h.call('PUT', `${ADMIN}/wide-api/operations`, admin, { operations: slice });
    assert.equal(fits.status, 200, JSON.stringify(fits.body).slice(0, 300));

    // A second connector of 20 operations (about 250 KB) is fine on its own, but not once it is assigned to developers.
    await h.call('POST', ADMIN, admin, { id: 'second-api', owner: 'it', name: 'Second', kind: 'api', base_url: fake.base, auth: 'none', groups: [] });
    await upload(h, admin, 'second-api', docText({ ...big, info: { ...big.info, title: 'Second API' } }));
    const twenty = up.body.operations.slice(30, 40).concat(up.body.operations.slice(0, 10)).map((o: any) => ({ key: o.key, selected: true }));
    const alone = await h.call('PUT', `${ADMIN}/second-api/operations`, admin, { operations: twenty });
    assert.equal(alone.status, 200, JSON.stringify(alone.body).slice(0, 300));
    const assign = await h.call('PUT', '/api/v1/admin/mcp/servers/second-api', admin, { groups: ['developers'] });
    assert.equal(assign.status, 400, 'assignment over the budget is refused');
    assert.equal(assign.body.error.code, 'BUDGET_EXCEEDED');
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: the admin list and the user listing show kinds and states without any secret', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    await createConnector(h, admin, fake, { ...PAT_CONNECTOR, id: 'jira-api', name: 'Jira' });
    await createConnector(h, admin, fake, { id: 'orders-api', name: 'Orders' });
    const adminList = await h.call('GET', ADMIN, admin);
    assert.equal(adminList.status, 200);
    const kinds = adminList.body.map((c: any) => [c.id, c.kind]).sort();
    assert.deepEqual(kinds, [['jira-api', 'api'], ['orders-api', 'api']]);
    const list = await h.call('GET', '/api/v1/connectors', dev1);
    assert.equal(list.body.connectors.length, 2);
    assert.doesNotMatch(JSON.stringify(list.body), /127\.0\.0\.1/, 'no base URL is listed');
    const outsider = await h.call('GET', '/api/v1/connectors', await h.token('outsider1'));
    assert.equal(outsider.body.connectors.length, 0, 'a user outside the Groups sees no connector');
    const denied = await h.call('GET', ADMIN, await h.token('dev1'));
    assert.equal(denied.status, 403, 'only a platform-admin reaches the administration API');
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: a disabled connector leaves the next bundle, an api-key needs its value, and the audit holds no key', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin, dev1 } = await setup(h);
    const secret = 'sk-audit-check-value-0001';
    await createConnector(h, admin, fake, { auth: 'api-key', key_header: 'X-Api-Key', api_key: secret });
    const noKey = await h.call('POST', ADMIN, admin, { id: 'keyless', owner: 'it', name: 'K', kind: 'api', base_url: fake.base, auth: 'api-key', key_header: 'X-Api-Key', groups: ['developers'] });
    assert.equal(noKey.status, 400, 'an api-key connector needs its key');
    assert.equal(noKey.body.error.field, 'api_key');
    await upload(h, admin, 'orders-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });
    await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '9' } });
    const audit = h.cp.db.prepare('SELECT details, reason FROM audit').all() as { details: string; reason: string | null }[];
    assert.ok(audit.length > 0);
    assert.ok(audit.every((r) => !String(r.details).includes(secret) && !String(r.reason ?? '').includes(secret)), 'no key value in the audit trail');
    assert.equal((h.cp.db.prepare("SELECT COUNT(*) AS n FROM audit WHERE action = 'api.key.set'").get() as { n: number }).n, 1, 'setting a key is audited without its value');

    assert.ok((await bundleServers(h, 'dev1')).some((s) => s.id === 'orders-api'));
    const off = await h.call('PUT', '/api/v1/admin/mcp/servers/orders-api', admin, { enabled: false });
    assert.equal(off.status, 200, JSON.stringify(off.body));
    assert.equal((await bundleServers(h, 'dev1')).some((s) => s.id === 'orders-api'), false, 'a disabled connector is absent from the next bundle');
    const gone = await h.call('POST', '/api/v1/connectors/orders-api/operations/getOrder/call', dev1, { arguments: { id: '9' } });
    assert.equal(gone.status, 403, 'and its calls are refused');
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: operations over 16 KB of schema and a 101st selected tool are refused', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin } = await setup(h);
    await createConnector(h, admin, fake);
    const wideParams = Array.from({ length: 700 }, (_, i) => ({ name: `p${String(i).padStart(3, '0')}`, in: 'query', schema: { type: 'string' } }));
    const doc = {
      openapi: '3.1.0',
      info: { title: 'Limits API', description: 'A document that tests the schema size and the number of selected tools per connector.' },
      paths: {
        '/wide': { get: { operationId: 'wideOne', description: 'An operation whose query schema is larger than the sixteen kilobyte limit.', parameters: wideParams } },
        ...Object.fromEntries(
          Array.from({ length: 101 }, (_, i) => [`/many${i}`, { get: { operationId: `many${i}`, description: `Operation number ${i} of the many operations used to reach the tool limit.` } }]),
        ),
      },
    };
    const up = await upload(h, admin, 'orders-api', docText(doc));
    assert.equal(up.status, 200, JSON.stringify(up.body).slice(0, 200));
    const wide = up.body.operations.find((o: any) => o.key === 'GET /wide');
    assert.equal(wide.usable, false);
    assert.match(wide.reason, /larger than 16 KB/);
    const hundredOne = up.body.operations.filter((o: any) => o.usable).map((o: any) => ({ key: o.key, selected: true }));
    assert.equal(hundredOne.length, 101);
    const refused = await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: hundredOne });
    assert.equal(refused.status, 400);
    assert.match(refused.body.error.message, /at most 100 tools/);
    assert.equal(up.body.counts.selected, 0, 'a refused save leaves the previous selection');
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: two Groups receive the connector once, and a user in neither Group gets nothing in the bundle', async () => {
  const fake = await fakeApi();
  const h = await startHarness(devOn);
  try {
    const { admin } = await setup(h);
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'sales1', workbench_group: 'sales' });
    await h.call('POST', '/api/v1/admin/identity/group-members', admin, { user_name: 'nobody1', workbench_group: 'other' });
    await createConnector(h, admin, fake, { groups: ['developers', 'sales'] });
    await upload(h, admin, 'orders-api', docText(DOC));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /orders/{id}', selected: true }] });
    const inDev = (await bundleServers(h, 'dev1')).filter((s) => s.id === 'orders-api');
    assert.equal(inDev.length, 1, 'the union of Groups gives one entry');
    const inSales = (await bundleServers(h, 'sales1')).filter((s) => s.id === 'orders-api');
    assert.equal(inSales.length, 1);
    assert.equal((await bundleServers(h, 'nobody1')).filter((s) => s.id === 'orders-api').length, 0, 'a user in neither Group gets nothing');
  } finally {
    await h.close();
    await fake.close();
  }
});

test('api connectors: a call that gets no answer in time ends with a timeout error', async () => {
  const fake = await fakeApi();
  const h = await startHarness((c) => {
    devOn(c);
    c.apiConnectors.callTimeoutMs = 150;
  });
  try {
    const { admin, dev1 } = await setup(h);
    await createConnector(h, admin, fake);
    const slow = {
      openapi: '3.1.0',
      info: { title: 'Slow API', description: 'A document whose one operation waits on the fake API so that the call times out.' },
      paths: { '/slow': { get: { operationId: 'slowOne', description: 'Waits for the fake API, which never answers in this test, so the call times out.' } } },
    };
    await upload(h, admin, 'orders-api', docText(slow));
    await h.call('PUT', `${ADMIN}/orders-api/operations`, admin, { operations: [{ key: 'GET /slow', selected: true }] });
    const timed = await h.call('POST', '/api/v1/connectors/orders-api/operations/slowOne/call', dev1, { arguments: {} });
    assert.equal(timed.status, 504);
    assert.equal(timed.body.error.code, 'TIMEOUT');
  } finally {
    fake.release();
    await h.close();
    await fake.close();
  }
});
