// Server settings (add-client-foundation design D10/D11). Everything comes from environment variables, so the same
// image runs in development (values from the repository's .env) and in production (values from the deployment).

export interface ModelCapability {
  kind: 'chat' | 'embed' | 'rerank' | 'image';
  context_window: number;
  max_output_tokens?: number;
  tools?: boolean;
  images?: boolean;
  reasoning?: boolean;
  dimension?: number;
}

export interface Config {
  env: 'development' | 'test' | 'production';
  port: number;
  host: string;
  /** Base URL the clients use; registration returns service URLs under it. Empty = derived from the request. */
  publicBaseUrl: string;
  dbPath: string;
  identity: {
    /** OpenID Connect discovery document of the identity provider. */
    discoveryUrl: string;
    /** Expected issuer; read from discovery when empty. */
    issuer: string;
    audiences: string[];
    allowedClients: string[];
    maxTokenLifetimeS: number;
    clockSkewS: number;
    bootstrapAdmins: string[];
    /**
     * Test only: an additional trusted issuer whose public JWKS is given inline. Refused in production. Used by
     * automated tests that need a second user or an expired token, which the real identity provider cannot give.
     */
    testIssuer?: { issuer: string; jwks: { keys: object[] } };
  };
  mcpCatalog: {
    /** Accept plain-HTTP remote entries (development and tests only). */
    allowInsecureHttp: boolean;
    cacheSeconds: number;
    maxPackageBytes: number;
    discoveryTimeoutMs: number;
  };
  /** control-plane/user-messaging (add-user-messaging): retention in days and the largest message. */
  messaging: { retentionDays: number; maxMessageBytes: number };
  /** control-plane/team-knowledge (add-knowledge-sharing D2). */
  teamKnowledge: { maxItemBytes: number; quotaBytes: number };
  /** control-plane/skill-registry (add-skills design D6). */
  skills: {
    maxPackageBytes: number;
    maxEntries: number;
    maxUnpackedBytes: number;
    revocationRetentionDays: number;
    /** Download public imports over plain HTTP (development and tests only). */
    allowInsecureDownload: boolean;
  };
  fleet: {
    heartbeatIntervalS: number;
    offlineAfterMissed: number;
    pushRetentionS: number;
    registryRetentionS: number;
    tokenExpiryGraceS: number;
    tokenCheckIntervalMs: number;
    maxClientMessageBytes: number;
    defaultChannel: string;
  };
  gateway: {
    /** Upstreams from the environment; they only bootstrap the first LLM connections (add-llm-connections D2). */
    upstreams: { name: string; baseUrl: string; apiKey: string; maxConcurrent: number }[];
    models: Record<string, string>;
    capabilities: Record<string, ModelCapability>;
    /** Key for LLM connection credentials at rest (WORKBENCH_SECRET_KEY, base64 of 32 bytes; add-llm-connections D1). */
    secretKey: string;
    /** Interval of the automatic model discovery (D4). */
    discoveryIntervalMs: number;
    budgetTokens: number;
    budgetPeriod: 'day' | 'week' | 'month';
    embedDailyTokens: number;
    /** Generated images per user per day (policy.llm.budget.images_daily default). */
    imagesDaily: number;
    requestsPerMinute: number;
    concurrentRequests: number;
    telemetryLevel: 'metadata' | 'content';
    maxRequestBytes: number;
    requestTimeoutMs: number;
    /** Image generation requests (add-image-generation design D5). */
    imageTimeoutMs: number;
    streamIdleTimeoutMs: number;
    healthCheckIntervalMs: number;
    imageTokenEstimate: number;
    maxAuditContentBytes: number;
  };
  policy: {
    /** Bundle validity (control-plane/policy-service Bundle Versioning And Expiry; default 7 days). */
    bundleValidityS: number;
  };
  telemetry: {
    /** OTLP/HTTP logs endpoint of the organization observability stack; empty = no forwarding. */
    forwardUrl: string;
    forwardHeaders: Record<string, string>;
    /** Replace user principals and machine names with keyed pseudonyms before forwarding (design D5). */
    pseudonymize: boolean;
    /** Key material for pseudonyms and crash-dump encryption (the secret store's value in production). */
    secret: string;
    retentionDays: number;
    crashRetentionDays: number;
    /** How long batches wait for an unavailable forwarding target (at least 24 hours). */
    forwardBufferHours: number;
    forwardIntervalMs: number;
    batchesPerMinute: number;
    batchBurst: number;
    maxBatchBytes: number;
    maxRecordsPerBatch: number;
    maxCrashBytes: number;
  };
}

const list = (v: string | undefined, fallback: string[]): string[] =>
  v === undefined || v.trim() === '' ? fallback : v.split(',').map((s) => s.trim()).filter(Boolean);
const num = (v: string | undefined, fallback: number): number => {
  const n = v === undefined || v === '' ? NaN : Number(v);
  return Number.isFinite(n) ? n : fallback;
};

export const defaultModels: Record<string, string> = {
  main: 'acme-llm-primary',
  vision: 'acme-llm-primary',
  small: 'acme-llm-secondary',
  embed: 'acme-llm-embedding',
  rerank: 'acme-llm-reranker',
  image: 'acme-llm-image',
};

// The upstream does not report capabilities (add-agent-runtime design D0), so they are server settings. The chat
// context windows are the ones measured by spike 1.2 (design D0), not the larger limits the upstream declares.
export const defaultCapabilities: Record<string, ModelCapability> = {
  'acme-llm-primary': { kind: 'chat', context_window: 131072, max_output_tokens: 8192, tools: true, images: true, reasoning: true },
  'acme-llm-secondary': { kind: 'chat', context_window: 65536, max_output_tokens: 4096, tools: true, images: false, reasoning: false },
  'acme-llm-embedding': { kind: 'embed', context_window: 8192, dimension: 4096 },
  'acme-llm-reranker': { kind: 'rerank', context_window: 8192 },
  'acme-llm-image': { kind: 'image', context_window: 0 },
};

export function loadConfig(e: NodeJS.ProcessEnv = process.env): Config {
  const env = (e.WORKBENCH_ENV ?? e.NODE_ENV ?? 'production') as Config['env'];
  const clientId = e.OIDC_CLIENT_ID ?? '';
  const upstreams: Config['gateway']['upstreams'] = [];
  // WORKBENCH_LLM_UPSTREAMS = "name=url|keyEnvVar,..." for several backends; otherwise OPENAI_BASE_URL/OPENAI_KEY.
  for (const spec of list(e.WORKBENCH_LLM_UPSTREAMS, [])) {
    const [name, rest] = spec.split('=');
    const [url, keyVar] = (rest ?? '').split('|');
    if (name && url) upstreams.push({ name, baseUrl: url, apiKey: keyVar ? (e[keyVar] ?? '') : '', maxConcurrent: 0 });
  }
  if (upstreams.length === 0 && e.OPENAI_BASE_URL) {
    upstreams.push({ name: 'primary', baseUrl: e.OPENAI_BASE_URL, apiKey: e.OPENAI_KEY ?? '', maxConcurrent: num(e.WORKBENCH_LLM_MAX_CONCURRENT, 0) });
  }
  let testIssuer: Config['identity']['testIssuer'];
  if (e.WORKBENCH_TEST_ISSUER && e.WORKBENCH_TEST_ISSUER_JWKS) {
    testIssuer = { issuer: e.WORKBENCH_TEST_ISSUER, jwks: JSON.parse(e.WORKBENCH_TEST_ISSUER_JWKS) };
  }
  return {
    env,
    port: num(e.PORT, 8080),
    host: e.HOST ?? '0.0.0.0',
    publicBaseUrl: (e.WORKBENCH_PUBLIC_BASE_URL ?? '').replace(/\/+$/, ''),
    dbPath: e.WORKBENCH_DB ?? 'data/workbench.db',
    identity: {
      discoveryUrl: e.OIDC_DISCOVERY_URL ?? '',
      issuer: e.OIDC_ISSUER ?? '',
      audiences: list(e.WORKBENCH_AUDIENCES, clientId ? [clientId] : []),
      allowedClients: list(e.WORKBENCH_ALLOWED_CLIENTS, clientId ? [clientId] : []),
      maxTokenLifetimeS: num(e.WORKBENCH_MAX_TOKEN_LIFETIME_S, 600),
      clockSkewS: 30,
      bootstrapAdmins: list(e.WORKBENCH_BOOTSTRAP_ADMINS, []).map((s) => s.toLowerCase()),
      testIssuer,
    },
    mcpCatalog: {
      allowInsecureHttp: e.WORKBENCH_MCP_ALLOW_INSECURE_HTTP === '1' || e.WORKBENCH_MCP_ALLOW_INSECURE_HTTP === 'true',
      cacheSeconds: num(e.WORKBENCH_MCP_CACHE_S, 5),
      maxPackageBytes: num(e.WORKBENCH_MCP_MAX_PACKAGE_BYTES, 64 * 1024 * 1024),
      discoveryTimeoutMs: num(e.WORKBENCH_MCP_DISCOVERY_TIMEOUT_MS, 10_000),
    },
    messaging: { retentionDays: num(e.WORKBENCH_MESSAGE_RETENTION_DAYS, 90), maxMessageBytes: 8192 },
    teamKnowledge: { maxItemBytes: num(e.WORKBENCH_TEAM_ITEM_MAX_BYTES, 2 * 1024 * 1024), quotaBytes: num(e.WORKBENCH_TEAM_QUOTA_BYTES, 200 * 1024 * 1024) },
    skills: {
      maxPackageBytes: num(e.WORKBENCH_SKILLS_MAX_PACKAGE_BYTES, 5 * 1024 * 1024),
      maxEntries: 200,
      maxUnpackedBytes: 20 * 1024 * 1024,
      revocationRetentionDays: num(e.WORKBENCH_SKILLS_REVOCATION_RETENTION_DAYS, 180),
      allowInsecureDownload: e.WORKBENCH_SKILLS_ALLOW_INSECURE_DOWNLOAD === '1' || e.WORKBENCH_SKILLS_ALLOW_INSECURE_DOWNLOAD === 'true',
    },
    fleet: {
      heartbeatIntervalS: num(e.WORKBENCH_HEARTBEAT_INTERVAL_S, 300),
      offlineAfterMissed: 3,
      pushRetentionS: 7 * 86400,
      registryRetentionS: 180 * 86400,
      tokenExpiryGraceS: num(e.WORKBENCH_TOKEN_EXPIRY_GRACE_S, 30),
      tokenCheckIntervalMs: num(e.WORKBENCH_TOKEN_CHECK_INTERVAL_MS, 5000),
      maxClientMessageBytes: 65536,
      defaultChannel: 'stable',
    },
    gateway: {
      upstreams,
      models: { ...defaultModels },
      capabilities: { ...defaultCapabilities },
      secretKey: e.WORKBENCH_SECRET_KEY ?? '',
      discoveryIntervalMs: 24 * 3600_000,
      budgetTokens: num(e.WORKBENCH_LLM_BUDGET_TOKENS, 1_000_000),
      budgetPeriod: 'day',
      embedDailyTokens: num(e.WORKBENCH_LLM_EMBED_DAILY_TOKENS, 12_000_000),
      imagesDaily: num(e.WORKBENCH_LLM_IMAGES_DAILY, 50),
      requestsPerMinute: num(e.WORKBENCH_LLM_RPM, 60),
      concurrentRequests: num(e.WORKBENCH_LLM_CONCURRENT, 4),
      telemetryLevel: e.WORKBENCH_TELEMETRY_LEVEL === 'content' ? 'content' : 'metadata',
      maxRequestBytes: 32 * 1024 * 1024,
      requestTimeoutMs: num(e.WORKBENCH_LLM_REQUEST_TIMEOUT_MS, 300_000),
      imageTimeoutMs: num(e.WORKBENCH_LLM_IMAGE_TIMEOUT_MS, 120_000),
      streamIdleTimeoutMs: num(e.WORKBENCH_LLM_STREAM_IDLE_MS, 120_000),
      healthCheckIntervalMs: num(e.WORKBENCH_LLM_HEALTH_INTERVAL_MS, 10_000),
      imageTokenEstimate: 1500,
      maxAuditContentBytes: 256 * 1024,
    },
    policy: {
      bundleValidityS: num(e.WORKBENCH_POLICY_BUNDLE_VALIDITY_S, 7 * 86400),
    },
    telemetry: {
      forwardUrl: e.WORKBENCH_TELEMETRY_FORWARD_URL ?? '',
      // "name=value,name=value", for example an API key header of the observability stack.
      forwardHeaders: Object.fromEntries(
        list(e.WORKBENCH_TELEMETRY_FORWARD_HEADERS, []).map((h) => [h.slice(0, h.indexOf('=')).trim(), h.slice(h.indexOf('=') + 1).trim()]).filter(([k]) => k),
      ),
      pseudonymize: e.WORKBENCH_TELEMETRY_PSEUDONYMIZE === '1' || e.WORKBENCH_TELEMETRY_PSEUDONYMIZE === 'true',
      secret: e.WORKBENCH_TELEMETRY_SECRET ?? (env === 'production' ? '' : 'workbench-development-telemetry-secret'),
      retentionDays: num(e.WORKBENCH_TELEMETRY_RETENTION_DAYS, 30),
      crashRetentionDays: Math.min(num(e.WORKBENCH_TELEMETRY_CRASH_RETENTION_DAYS, 90), 90),
      forwardBufferHours: Math.max(num(e.WORKBENCH_TELEMETRY_FORWARD_BUFFER_HOURS, 48), 24),
      forwardIntervalMs: num(e.WORKBENCH_TELEMETRY_FORWARD_INTERVAL_MS, 15_000),
      batchesPerMinute: num(e.WORKBENCH_TELEMETRY_BATCHES_PER_MINUTE, 6),
      batchBurst: num(e.WORKBENCH_TELEMETRY_BATCH_BURST, 20),
      maxBatchBytes: 1024 * 1024,
      maxRecordsPerBatch: 1000,
      maxCrashBytes: 8 * 1024 * 1024,
    },
  };
}

/** Production startup guard (control-plane/identity Production Issuer Guard). Returns the reason to refuse, if any. */
export function productionGuard(c: Config): string | null {
  if (c.env !== 'production') return null;
  const issuerish = c.identity.issuer || c.identity.discoveryUrl;
  if (!issuerish) return 'no identity provider is configured (OIDC_DISCOVERY_URL)';
  if (!issuerish.startsWith('https://')) return `the identity provider must use https: ${issuerish}`;
  if (c.identity.testIssuer) return 'a test issuer is configured, which production refuses';
  if (!c.telemetry.secret) return 'no telemetry secret is configured (WORKBENCH_TELEMETRY_SECRET)';
  return null;
}
