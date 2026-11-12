// Spike for add-policy-and-sandbox tasks 1.1 and 1.2: which common tools honour the proxy variables the sandbox sets
// (client/src/sandbox/sandbox.cpp). A local proxy refuses every request, like no-network mode, and records who asked.
// A tool that never reaches the proxy ignores the variables. Usage: node dev/scripts/proxy-probe.mjs
import http from 'node:http';
import { spawn } from 'node:child_process';

// Asynchronous on purpose: the proxy lives in this process and must keep answering while the tool runs.
const run = (cmd, args, opts) => new Promise((resolve) => {
  let out = '';
  const p = spawn(cmd, args, opts);
  p.stdout.on('data', (d) => (out += d));
  p.stderr.on('data', (d) => (out += d));
  const t = setTimeout(() => p.kill(), 30000);
  p.on('error', (error) => { clearTimeout(t); resolve({ error, out }); });
  p.on('close', () => { clearTimeout(t); resolve({ out }); });
});

let seen = [];
const proxy = http.createServer((req, res) => { seen.push(`${req.method} ${req.url}`); res.writeHead(403).end('refused'); });
proxy.on('connect', (req, socket) => { seen.push(`CONNECT ${req.url}`); socket.end('HTTP/1.1 403 Forbidden\r\n\r\n'); });
await new Promise((r) => proxy.listen(0, '127.0.0.1', r));
const url = `http://probe:secret@127.0.0.1:${proxy.address().port}`;
const env = { ...process.env, NO_PROXY: '', no_proxy: '', NODE_USE_ENV_PROXY: '1' };
for (const k of ['HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'http_proxy', 'https_proxy', 'all_proxy']) env[k] = url;

const target = 'https://example.com/';
const tools = [
  ['git', 'git', ['ls-remote', 'https://example.com/probe.git']],
  ['curl', 'curl', ['-s', '-m', '10', target]],
  ['npm', 'npm.cmd', ['view', 'left-pad', 'version', '--fetch-retries=0', '--fetch-timeout=10000']],
  ['node fetch', 'node', ['-e', `fetch('${target}').then(r=>console.log('direct',r.status),e=>console.log('failed',e.cause?.code||e.message))`]],
  ['node https', 'node', ['-e', `require('https').get('${target}',r=>{console.log('direct',r.statusCode);r.resume()}).on('error',e=>console.log('failed',e.code))`]],
  ['python urllib', 'python', ['-c', `import urllib.request as u\ntry:\n print('direct', u.urlopen('${target}', timeout=10).status)\nexcept Exception as e:\n print('failed', e)`]],
  ['pwsh 7 Invoke-WebRequest', 'pwsh', ['-NoProfile', '-Command', `try { 'direct ' + (Invoke-WebRequest '${target}' -TimeoutSec 10).StatusCode } catch { 'failed ' + $_.Exception.Message }`]],
  ['powershell 5.1 Invoke-WebRequest', 'powershell', ['-NoProfile', '-Command', `try { 'direct ' + (Invoke-WebRequest '${target}' -UseBasicParsing -TimeoutSec 10).StatusCode } catch { 'failed ' + $_.Exception.Message }`]],
  ['dotnet HttpClient (via pwsh)', 'pwsh', ['-NoProfile', '-Command', `try { 'direct ' + [int][System.Net.Http.HttpClient]::new().GetAsync('${target}').Result.StatusCode } catch { 'failed ' + $_.Exception.InnerException.Message }`]],
];
for (const [name, cmd, args] of tools) {
  seen = [];
  const r = await run(cmd, args, { env, shell: cmd.endsWith('.cmd') });
  const out = r.out.replace(/\s+/g, ' ').trim().slice(0, 110);
  const verdict = r.error?.code === 'ENOENT' ? 'not installed' : seen.length ? 'uses the proxy (refused)' : 'IGNORES the proxy variables';
  console.log(`${name.padEnd(34)} ${verdict.padEnd(28)} ${out}`);
}
proxy.close();
