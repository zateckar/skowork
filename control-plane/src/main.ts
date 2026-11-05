// Entry point: node src/main.ts (Node.js runs the TypeScript sources directly).
import { loadConfig, productionGuard } from './config.ts';
import { createControlPlane } from './app.ts';
import { setupReport } from './setup-report.ts';

const cfg = loadConfig();
const refusal = productionGuard(cfg);
if (refusal) {
  console.error(`refusing to start: ${refusal}`);
  process.exit(1);
}
const cp = createControlPlane(cfg);
const port = await cp.listen();
console.log(`Workbench control plane (${cfg.env}) listening on ${cfg.host}:${port}`);
const report = setupReport(cfg, { consoleBuilt: cp.consoleBuilt, port });
for (const line of report.info) console.log(line);
for (const line of report.warnings) console.warn(line);
if (report.warnings.length > 0) console.warn('see "Deploy the control plane" in README.md for the settings');

const stop = async () => {
  await cp.close();
  process.exit(0);
};
process.on('SIGINT', stop);
process.on('SIGTERM', stop);
