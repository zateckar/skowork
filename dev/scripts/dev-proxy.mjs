// Development forward proxy (HTTP CONNECT only) on 127.0.0.1. Some managed machines block outbound connections
// from unsigned executables, such as a locally built Workbench.exe or e2e_tests.exe. Debug and test builds can then
// reach the real identity provider through this proxy (WORKBENCH_DEV_PROXY=http://127.0.0.1:3128). It only tunnels
// bytes: TLS stays end to end and nothing is logged except host names.
//
//   node dev/scripts/dev-proxy.mjs [port]
import net from 'node:net';
import http from 'node:http';

const port = Number(process.argv[2] ?? process.env.WORKBENCH_DEV_PROXY_PORT ?? 3128);

const server = http.createServer((_req, res) => {
  res.writeHead(405, { 'content-type': 'text/plain' }).end('CONNECT only\n');
});

server.on('connect', (req, client, head) => {
  const [host, portText] = String(req.url).split(':');
  const target = Number(portText);
  if (!host || target !== 443) {
    client.end('HTTP/1.1 403 Forbidden\r\n\r\n');
    return;
  }
  const upstream = net.connect(target, host, () => {
    client.write('HTTP/1.1 200 Connection Established\r\n\r\n');
    if (head?.length) upstream.write(head);
    upstream.pipe(client);
    client.pipe(upstream);
  });
  const close = () => {
    upstream.destroy();
    client.destroy();
  };
  upstream.on('error', (e) => {
    console.error(`dev-proxy: ${host}: ${e.code ?? e.message}`);
    if (!client.destroyed) client.end('HTTP/1.1 502 Bad Gateway\r\n\r\n');
    close();
  });
  client.on('error', close);
  console.log(`dev-proxy: CONNECT ${host}:${target}`);
});

server.listen(port, '127.0.0.1', () => console.log(`dev-proxy listening on http://127.0.0.1:${port}`));
