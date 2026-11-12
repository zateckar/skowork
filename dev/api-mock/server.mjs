// Dev-only fake API for API connectors (openspec add-api-connectors, task 8.1). Never shipped.
// Dependency-free: node:http only. Serves the OpenAPI 3.1 document in openapi.json at /openapi.json, so it can be
// uploaded in the admin console, and answers its operations on 127.0.0.1 only.
//   node dev/api-mock/server.mjs            (port from API_MOCK_PORT, default 4010)
// Operations: getOrder (read), createOrder (write, POST), legacyStatus (bad description "TODO", not usable).
// The request log prints method, path and whether an Authorization header arrived, never its value.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';

const doc = JSON.parse(await readFile(new URL('./openapi.json', import.meta.url), 'utf8'));
const port = Number(process.env.API_MOCK_PORT ?? 4010);
const orders = new Map([
  ['ord_1001', { id: 'ord_1001', customerId: 'cus_1', status: 'shipped', lines: [{ sku: 'SKU-1', quantity: 2 }], total: 39.9 }],
]);
let next = 1002;

const send = (res, status, body) => {
  res.writeHead(status, { 'content-type': 'application/json; charset=utf-8' });
  res.end(JSON.stringify(body));
};

const readBody = (req) =>
  new Promise((resolve, reject) => {
    let text = '';
    req.setEncoding('utf8');
    req.on('data', (chunk) => {
      text += chunk;
      if (text.length > 64 * 1024) reject(new Error('body too large'));
    });
    req.on('end', () => resolve(text));
    req.on('error', reject);
  });

const server = createServer(async (req, res) => {
  const url = new URL(req.url ?? '/', `http://127.0.0.1:${port}`);
  console.log(`${new Date().toISOString()} ${req.method} ${url.pathname} auth=${req.headers.authorization ? 'present' : 'none'}`);

  if (req.method === 'GET' && url.pathname === '/openapi.json') return send(res, 200, doc);

  if (req.method === 'GET' && url.pathname === '/legacy/status') return send(res, 200, { status: 'ok' });

  const one = url.pathname.match(/^\/orders\/([^/]+)$/);
  if (req.method === 'GET' && one) {
    const order = orders.get(decodeURIComponent(one[1]));
    return order ? send(res, 200, order) : send(res, 404, { error: 'not found' });
  }

  if (req.method === 'POST' && url.pathname === '/orders') {
    let input;
    try {
      input = JSON.parse(await readBody(req));
    } catch {
      return send(res, 400, { error: 'body must be JSON' });
    }
    const valid =
      input && typeof input.customerId === 'string' && input.customerId.length > 0 &&
      Array.isArray(input.lines) && input.lines.length > 0 &&
      input.lines.every((l) => l && typeof l.sku === 'string' && Number.isInteger(l.quantity) && l.quantity >= 1);
    if (!valid) return send(res, 400, { error: 'customerId and lines are required' });
    const order = { id: `ord_${next++}`, customerId: input.customerId, status: 'open', lines: input.lines, total: input.lines.length * 9.5 };
    orders.set(order.id, order);
    return send(res, 201, order);
  }

  send(res, 404, { error: 'no such route' });
});

server.listen(port, '127.0.0.1', () => console.log(`api-mock listening on http://127.0.0.1:${port}`));
