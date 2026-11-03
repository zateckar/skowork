// Packages the mock as a single executable (Node.js single executable application): one CommonJS bundle with the
// fixtures, then `node --build-sea`. Output: dist/graph-mock(.exe). With --check it also starts the executable and
// fails unless it is ready within 5 seconds.
import { build } from 'esbuild';
import { execFileSync, spawn } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '..');
const dist = join(root, 'dist');
mkdirSync(dist, { recursive: true });
const bundle = join(dist, 'graph-mock.cjs');
const exe = join(dist, process.platform === 'win32' ? 'graph-mock.exe' : 'graph-mock');

await build({ entryPoints: [join(root, 'src/main.ts')], bundle: true, platform: 'node', format: 'cjs', target: 'node24', outfile: bundle, logLevel: 'warning' });
const config = join(dist, 'sea-config.json');
writeFileSync(config, JSON.stringify({ main: bundle, output: exe, disableExperimentalSEAWarning: true, useCodeCache: false, useSnapshot: false }, null, 2));
execFileSync(process.execPath, ['--build-sea', config], { stdio: 'inherit' });
process.stdout.write(`built ${exe}\n`);

if (process.argv.includes('--check')) {
  const started = Date.now();
  const child = spawn(exe, ['--port', '0', '--control-port', '0'], { stdio: ['ignore', 'pipe', 'inherit'] });
  const line = await new Promise<string>((ok, fail) => {
    const timer = setTimeout(() => fail(new Error('not ready within 5 seconds')), 5000);
    let buf = '';
    child.stdout.on('data', (d: Buffer) => {
      buf += d.toString();
      const nl = buf.indexOf('\n');
      if (nl >= 0) {
        clearTimeout(timer);
        ok(buf.slice(0, nl));
      }
    });
    child.once('exit', (code) => fail(new Error(`exited with ${code}`)));
  });
  const ready = Date.now() - started;
  const { control } = JSON.parse(line) as { control: string };
  const health = (await (await fetch(`${control}/_mock/health`)).json()) as { ok: boolean };
  child.kill();
  if (!health.ok) throw new Error('health check failed');
  process.stdout.write(`ready in ${ready} ms\n`);
}
