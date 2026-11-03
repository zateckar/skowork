// Tool discovery of a remote MCP server over Streamable HTTP (add-core-tools-and-mcp design D6): initialize,
// notifications/initialized, then tools/list page by page. Answers may be plain JSON or an event stream.

export interface DiscoveredTool {
  name: string;
  description: string;
  annotations: Record<string, unknown>;
}

export const MCP_PROTOCOL_VERSION = '2025-06-18';

export class DiscoveryError extends Error {}

/** Reads the JSON-RPC response with the given id from a JSON body or a text/event-stream body. */
function pickResponse(contentType: string, text: string, id: number): any {
  const messages: any[] = [];
  if (contentType.includes('text/event-stream')) {
    for (const event of text.split(/\r?\n\r?\n/)) {
      const data = event
        .split(/\r?\n/)
        .filter((l) => l.startsWith('data:'))
        .map((l) => l.slice(5).replace(/^ /, ''))
        .join('\n');
      if (!data) continue;
      try {
        messages.push(JSON.parse(data));
      } catch {
        /* not JSON: ignore the event */
      }
    }
  } else {
    let body: unknown;
    try {
      body = JSON.parse(text);
    } catch {
      throw new DiscoveryError('the server did not answer with JSON');
    }
    messages.push(...(Array.isArray(body) ? body : [body]));
  }
  const msg = messages.find((m) => m && typeof m === 'object' && m.id === id);
  if (!msg) throw new DiscoveryError('the server did not answer the request');
  if (msg.error) throw new DiscoveryError(`the server returned error ${msg.error.code}: ${msg.error.message ?? ''}`.trim());
  return msg.result ?? {};
}

export async function discoverTools(url: string, opts: { token?: string; timeoutMs: number; specVersion?: string }): Promise<DiscoveredTool[]> {
  const deadline = AbortSignal.timeout(opts.timeoutMs);
  let session: string | null = null;
  let protocol = opts.specVersion || MCP_PROTOCOL_VERSION;
  let nextId = 1;
  let negotiated = false;

  const post = async (body: object, expectAnswer: boolean): Promise<any> => {
    const headers: Record<string, string> = { 'content-type': 'application/json', accept: 'application/json, text/event-stream' };
    if (opts.token) headers.authorization = `Bearer ${opts.token}`;
    if (session) headers['mcp-session-id'] = session;
    if (negotiated) headers['mcp-protocol-version'] = protocol;
    let res: Response;
    try {
      res = await fetch(url, { method: 'POST', headers, body: JSON.stringify(body), signal: deadline, redirect: 'error' });
    } catch (err) {
      const e = err as Error & { cause?: { code?: string; message?: string } };
      throw new DiscoveryError(`the server could not be reached: ${e.cause?.code ?? e.cause?.message ?? e.message}`);
    }
    const text = await res.text();
    if (!res.ok && !(res.status === 202 && !expectAnswer)) {
      throw new DiscoveryError(`the server answered HTTP ${res.status}`);
    }
    session = res.headers.get('mcp-session-id') ?? session;
    if (!expectAnswer) return null;
    return pickResponse(res.headers.get('content-type') ?? '', text, (body as { id: number }).id);
  };

  const init = await post(
    { jsonrpc: '2.0', id: nextId++, method: 'initialize', params: { protocolVersion: protocol, capabilities: {}, clientInfo: { name: 'workbench-catalog-discovery', version: '1.0' } } },
    true,
  );
  if (typeof init.protocolVersion !== 'string') throw new DiscoveryError('the server did not complete the MCP handshake');
  protocol = init.protocolVersion;
  negotiated = true;
  await post({ jsonrpc: '2.0', method: 'notifications/initialized' }, false);

  const tools: DiscoveredTool[] = [];
  let cursor: string | undefined;
  for (let page = 0; page < 100; page++) {
    const r = await post({ jsonrpc: '2.0', id: nextId++, method: 'tools/list', params: cursor ? { cursor } : {} }, true);
    for (const t of Array.isArray(r.tools) ? r.tools : []) {
      if (t && typeof t.name === 'string') {
        tools.push({ name: t.name, description: typeof t.description === 'string' ? t.description : '', annotations: t.annotations && typeof t.annotations === 'object' ? t.annotations : {} });
      }
    }
    cursor = typeof r.nextCursor === 'string' && r.nextCursor ? r.nextCursor : undefined;
    if (!cursor) break;
  }
  // Best effort: end the session.
  if (session) {
    void fetch(url, { method: 'DELETE', headers: { 'mcp-session-id': session, ...(opts.token ? { authorization: `Bearer ${opts.token}` } : {}) }, signal: AbortSignal.timeout(2000) }).catch(() => {});
  }
  return tools;
}
