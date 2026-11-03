// Routes of control-plane/telemetry-ingest: the client endpoint (signed-in users) and the operational counters (admin).
import express, { Router } from 'express';
import type { Config } from '../config.ts';
import { requireRole } from '../identity/identity.ts';
import type { TelemetryIngest } from './ingest.ts';

export function telemetryRoutes(cfg: Config['telemetry'], ingest: TelemetryIngest): Router {
  const r = Router();
  r.post('/api/v1/telemetry/v1/logs', express.raw({ type: () => true, limit: cfg.maxBatchBytes }), (req, res) => ingest.handleLogs(req, res));
  r.post('/api/v1/telemetry/v1/crashes', express.raw({ type: () => true, limit: cfg.maxCrashBytes + 1024 }), (req, res) => ingest.handleCrash(req, res));
  return r;
}

export function telemetryAdminRoutes(ingest: TelemetryIngest): Router {
  const r = Router();
  r.get('/stats', requireRole('platform-admin'), (_req, res) => {
    res.json({ ...ingest.stats, outbox: ingest.outboxSize() });
  });
  return r;
}
