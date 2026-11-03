// Shared HTTP helpers: the error envelope of design D10 and small request helpers.
import type { Request, Response } from 'express';

export interface ErrorExtra {
  reason?: string;
  retry_after_s?: number;
  reset_at?: string;
  /** Structured details, for example the validation errors of a policy document. */
  [key: string]: unknown;
}

export function sendError(res: Response, status: number, code: string, message: string, extra: ErrorExtra = {}): void {
  if (extra.retry_after_s !== undefined) res.setHeader('Retry-After', String(extra.retry_after_s));
  res.status(status).json({ error: { code, message, ...extra } });
}

export class ApiError extends Error {
  readonly status: number;
  readonly code: string;
  readonly extra: ErrorExtra;
  constructor(status: number, code: string, message: string, extra: ErrorExtra = {}) {
    super(message);
    this.status = status;
    this.code = code;
    this.extra = extra;
  }
}

export const clientIp = (req: Request): string => req.socket.remoteAddress ?? '';

export function str(v: unknown): string | undefined {
  return typeof v === 'string' ? v : undefined;
}
