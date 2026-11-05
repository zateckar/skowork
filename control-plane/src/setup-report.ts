// Deployment Setup Report (control-plane/admin-console, improve-deployment-guidance): what an operator still has to
// set before admins and clients can sign in and connect. Warnings only; the startup guard is productionGuard().
import type { Config } from './config.ts';

export interface SetupReport {
  info: string[];
  warnings: string[];
}

export function setupReport(cfg: Config, opts: { consoleBuilt: boolean; port: number }): SetupReport {
  const info: string[] = [];
  const warnings: string[] = [];
  const base = cfg.publicBaseUrl || `http://${cfg.host === '0.0.0.0' ? 'localhost' : cfg.host}:${opts.port}`;
  if (opts.consoleBuilt) info.push(`admin console: ${base}/admin/`);
  else warnings.push('warning: the admin console is not built (dist/admin is missing); run "npm run admin:build" or use the container image');
  if (cfg.identity.allowedClients.length === 0)
    warnings.push('warning: no sign-in client is configured (OIDC_CLIENT_ID); every token is refused, so neither the admin console nor the desktop clients can sign in');
  if (!cfg.publicBaseUrl)
    warnings.push('warning: WORKBENCH_PUBLIC_BASE_URL is not set; registration derives service URLs from each request, which gives http:// URLs behind a TLS proxy, and desktop clients refuse them');
  if (cfg.identity.bootstrapAdmins.length === 0)
    warnings.push('warning: WORKBENCH_BOOTSTRAP_ADMINS is empty; unless an admin was assigned earlier, nobody can use the admin console');
  if (!cfg.gateway.secretKey && cfg.env === 'production')
    warnings.push('warning: WORKBENCH_SECRET_KEY is not set; LLM connections cannot be added in the admin console (generate one with: openssl rand -base64 32)');
  if (cfg.gateway.upstreams.length === 0)
    warnings.push('warning: no LLM upstream configured (OPENAI_BASE_URL); the gateway will answer MODEL_UNAVAILABLE until an LLM connection is added in the admin console');
  return { info, warnings };
}
