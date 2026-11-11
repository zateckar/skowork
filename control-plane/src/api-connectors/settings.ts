// control-plane/api-connectors: the connection settings of an API connector (Connector Authentication, Personal Access
// Token Settings, Secret Handling). The catalog and the admin API check them the same way (design D6, D10, D11).
import { ApiError } from '../http.ts';
import { collapse } from './rules.ts';

export const AUTH_MODES = ['none', 'api-key', 'user-token', 'user-pat'] as const;
export type ApiAuth = (typeof AUTH_MODES)[number];

/** Headers that carry framing or hop-by-hop meaning and can never hold a credential. */
const FORBIDDEN_HEADERS = new Set(['connection', 'host', 'content-length', 'transfer-encoding', 'keep-alive', 'te', 'trailer', 'upgrade', 'cookie', 'proxy-authorization']);

export interface ApiSettings {
  base_url: string;
  auth: ApiAuth;
  key_header?: string;
  key_scheme?: string;
  help_text?: string;
  help_link?: string;
}

const invalid = (field: string, message: string) => new ApiError(400, 'VALIDATION_FAILED', message, { field });

/** True for loopback, link-local and cloud-metadata addresses and names (Upstream Request Rules, design D10). */
export function isLinkLocalOrMetadata(host: string): boolean {
  const h = host.replace(/^\[|\]$/g, '').toLowerCase();
  if (/^169\.254\./.test(h) || /^fe[89ab][0-9a-f]?:/.test(h) || h === 'fd00:ec2::254') return true;
  if (h === 'metadata.google.internal' || h === 'metadata' || h === 'instance-data') return true;
  return false;
}

export function isLoopback(host: string): boolean {
  const h = host.replace(/^\[|\]$/g, '').toLowerCase();
  return h === 'localhost' || h === '::1' || /^127\./.test(h);
}

/** The base URL: HTTPS, or HTTP to a loopback address when the development setting allows it. */
export function checkBaseUrl(raw: unknown, allowLoopbackHttp: boolean): string {
  let url: URL;
  try {
    url = new URL(String(raw ?? ''));
  } catch {
    throw invalid('base_url', 'An API connector needs a valid base URL.');
  }
  if (url.username || url.password) throw invalid('base_url', 'The base URL must not contain a user name or password.');
  if (url.search || url.hash) throw invalid('base_url', 'The base URL must not contain a query string or a fragment.');
  if (isLinkLocalOrMetadata(url.hostname)) throw invalid('base_url', `The address ${url.hostname} is a link-local or metadata address and is refused.`);
  if (url.protocol === 'https:') return url.href;
  if (url.protocol === 'http:' && allowLoopbackHttp && isLoopback(url.hostname)) return url.href;
  throw invalid('base_url', `The base URL ${url.origin} must use HTTPS.`);
}

/** Checks the settings of an API connector. `body` may hold other fields; only the settings are read. */
export function validateApiSettings(body: Record<string, unknown>, allowLoopbackHttp: boolean): ApiSettings {
  const base_url = checkBaseUrl(body.base_url, allowLoopbackHttp);
  const auth = body.auth ?? 'none';
  if (typeof auth !== 'string' || !(AUTH_MODES as readonly string[]).includes(auth)) {
    throw invalid('auth', `auth must be one of ${AUTH_MODES.join(', ')}.`);
  }
  if (body.key_in !== undefined && body.key_in !== 'header') throw invalid('key_in', 'The key must be sent in a header, not in the query string or the URL.');
  const out: ApiSettings = { base_url, auth: auth as ApiAuth };
  if (auth === 'api-key' || auth === 'user-pat') {
    const header = typeof body.key_header === 'string' ? body.key_header.trim() : '';
    if (!/^[A-Za-z0-9-]{1,64}$/.test(header)) throw invalid('key_header', 'The header name must be 1 to 64 letters, digits or hyphens.');
    if (FORBIDDEN_HEADERS.has(header.toLowerCase())) throw invalid('key_header', `The header ${header} cannot carry a credential.`);
    out.key_header = header;
  }
  if (auth === 'user-pat') {
    const scheme = typeof body.key_scheme === 'string' ? body.key_scheme.trim() : '';
    if (scheme && !/^[A-Za-z]{1,32}$/.test(scheme)) throw invalid('key_scheme', 'The scheme must be empty or 1 to 32 letters, for example Bearer.');
    out.key_scheme = scheme || undefined;
    const help = typeof body.help_text === 'string' ? collapse(body.help_text) : '';
    if (help.length < 20 || help.length > 500) {
      throw invalid('help_text', 'Users need help text of 20 to 500 characters that tells them where to create a token.');
    }
    out.help_text = help;
    if (body.help_link !== undefined && body.help_link !== null && body.help_link !== '') {
      let link: URL;
      try {
        link = new URL(String(body.help_link));
      } catch {
        throw invalid('help_link', 'The help link must be a valid HTTPS address.');
      }
      if (link.protocol !== 'https:' || link.username || link.password) throw invalid('help_link', 'The help link must be an HTTPS address without a user name.');
      out.help_link = link.href;
    }
  }
  return out;
}
