// Runs the opt-in native acceptance against a real organization model through an isolated local control plane.
// Usage: node --env-file=.env openspec/changes/add-generative-thread-ui/gateway-acceptance.mjs
// The test uses synthetic data and a mock mail Tool. Credentials and the short-lived test token are not logged.
import { spawn, spawnSync } from 'node:child_process';
import { startHarness } from '../../../control-plane/test/helpers.ts';
import { KEYS } from '../../../control-plane/src/policy/keys.ts';

if (!process.env.OPENAI_BASE_URL || !process.env.OPENAI_KEY) throw new Error('Organization model configuration missing');
const mapping = { main: process.env.OPENAI_MODEL || 'skoda-llm-primary', small: 'skoda-llm-secondary',
  vision: process.env.OPENAI_MODEL || 'skoda-llm-primary', embed: 'skoda-llm-embedding',
  rerank: 'skoda-llm-reranker', image: 'skoda-llm-image' };
KEYS.find(key => key.path === 'llm.models').default = mapping;
const harness = await startHarness(config => {
  config.gateway.upstreams = [{ name: 'organization', baseUrl: process.env.OPENAI_BASE_URL,
    apiKey: process.env.OPENAI_KEY, maxConcurrent: 0 }];
  config.gateway.models = { ...mapping };
});
try {
  for (let i = 0; i < 100; ++i) {
    const connections = harness.cp.gateway.connections.list().connections;
    if (connections.length && connections.every(connection => connection.status === 'ok')) break;
    if (i === 99) throw new Error('Gateway model discovery unavailable');
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  const token = await harness.token('genui-acceptance', {}, { lifetimeS: 600 });
  const probe = await harness.call('GET', '/llm/v1/models', token);
  if (probe.status !== 200) throw new Error(`Gateway authentication probe failed: HTTP ${probe.status}`);
  const listed = spawnSync('client/build/debug/bin/win_tests.exe',
    ['--gtest_list_tests', '--gtest_filter=ShellLiveGeneratedSurface.RealGatewayInteraction'], { encoding: 'utf8' });
  if (listed.status !== 0 || !listed.stdout.includes('RealGatewayInteraction')) throw new Error('Native acceptance test is not built');
  const child = spawn('client/build/debug/bin/win_tests.exe',
    ['--gtest_filter=ShellLiveGeneratedSurface.RealGatewayInteraction'], {
      stdio: 'inherit', env: { ...process.env, SKOWORK_TEST_GATEWAY: `${harness.base}/llm/v1`,
        SKOWORK_TEST_GATEWAY_TOKEN: token },
    });
  const code = await new Promise((resolve, reject) => {
    child.on('error', reject);
    child.on('exit', (status, signal) => resolve(signal ? 1 : status ?? 1));
  });
  process.exitCode = code;
} finally {
  await harness.close();
}
