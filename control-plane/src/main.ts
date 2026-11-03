// Entry point: node src/main.ts (Node.js runs the TypeScript sources directly).
import { loadConfig, productionGuard } from './config.ts';
import { createControlPlane } from './app.ts';

const cfg = loadConfig();
const refusal = productionGuard(cfg);
if (refusal) {
  console.error(`refusing to start: ${refusal}`);
  process.exit(1);
}
if (cfg.gateway.upstreams.length === 0) console.warn('warning: no LLM upstream configured (OPENAI_BASE_URL); the gateway will answer MODEL_UNAVAILABLE');

const cp = createControlPlane(cfg);
const port = await cp.listen();
console.log(`Workbench control plane (${cfg.env}) listening on ${cfg.host}:${port}`);

const stop = async () => {
  await cp.close();
  process.exit(0);
};
process.on('SIGINT', stop);
process.on('SIGTERM', stop);
