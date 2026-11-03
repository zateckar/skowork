// Generated from telemetry/catalogue.yaml by control-plane/scripts/telemetry-catalogue.ts. Do not edit.
// The telemetry allowlist (add-telemetry-and-admin design D1), re-applied by the ingest.

export type Level = 'metadata' | 'content';
export type FieldType = 'string' | 'integer' | 'number' | 'boolean' | 'domain';
export interface FieldDef {
  readonly type: FieldType;
  readonly level: Level;
}

export const RESOURCE = {
  'service.version': { type: 'string', level: 'metadata' },
  'workbench.channel': { type: 'string', level: 'metadata' },
  'os.build': { type: 'string', level: 'metadata' },
  'workbench.installation_id': { type: 'string', level: 'metadata' },
} as const satisfies Record<string, FieldDef>;
export type ResourceName = keyof typeof RESOURCE;

export const ATTRIBUTES = {
  'run.id': { type: 'string', level: 'metadata' },
  'run.outcome': { type: 'string', level: 'metadata' },
  'run.origin': { type: 'string', level: 'metadata' },
  'run.duration_ms': { type: 'integer', level: 'metadata' },
  'run.tool_calls': { type: 'integer', level: 'metadata' },
  'tool.id': { type: 'string', level: 'metadata' },
  'tool.risk': { type: 'string', level: 'metadata' },
  'tool.outcome': { type: 'string', level: 'metadata' },
  'tool.duration_ms': { type: 'integer', level: 'metadata' },
  'url.domain': { type: 'domain', level: 'metadata' },
  'approval.decision': { type: 'string', level: 'metadata' },
  'policy.rule': { type: 'string', level: 'metadata' },
  'policy.version': { type: 'integer', level: 'metadata' },
  'policy.outcome': { type: 'string', level: 'metadata' },
  'model.role': { type: 'string', level: 'metadata' },
  'tokens.prompt': { type: 'integer', level: 'metadata' },
  'tokens.completion': { type: 'integer', level: 'metadata' },
  'error.code': { type: 'string', level: 'metadata' },
  'error.component': { type: 'string', level: 'metadata' },
  'network.enforcement': { type: 'string', level: 'metadata' },
  'process.startup_ms': { type: 'integer', level: 'metadata' },
  'process.working_set_mb': { type: 'number', level: 'metadata' },
  'telemetry.discarded': { type: 'integer', level: 'metadata' },
  'crash.id': { type: 'string', level: 'metadata' },
  'crash.process': { type: 'string', level: 'metadata' },
  'crash.exception_code': { type: 'string', level: 'metadata' },
  'crash.module': { type: 'string', level: 'metadata' },
  'crash.dump_bytes': { type: 'integer', level: 'metadata' },
  'content.prompt': { type: 'string', level: 'content' },
  'content.response': { type: 'string', level: 'content' },
  'tool.arguments': { type: 'string', level: 'content' },
  'error.message': { type: 'string', level: 'content' },
} as const satisfies Record<string, FieldDef>;
export type AttrName = keyof typeof ATTRIBUTES;

export const EVENTS = {
  'run.finished': ['run.id', 'run.outcome', 'run.origin', 'run.duration_ms', 'run.tool_calls', 'error.code', 'content.prompt', 'content.response'],
  'tool.call': ['run.id', 'tool.id', 'tool.risk', 'tool.outcome', 'tool.duration_ms', 'url.domain', 'tool.arguments'],
  'approval.resolved': ['run.id', 'tool.id', 'tool.risk', 'approval.decision'],
  'policy.decision': ['run.id', 'tool.id', 'tool.risk', 'policy.rule', 'policy.version', 'policy.outcome'],
  'model.usage': ['run.id', 'model.role', 'tokens.prompt', 'tokens.completion', 'error.code'],
  'client.error': ['error.code', 'error.component', 'error.message'],
  'client.started': ['process.startup_ms', 'network.enforcement'],
  'client.footprint': ['process.working_set_mb'],
  'sandbox.enforcement': ['network.enforcement'],
  'telemetry.buffer': ['telemetry.discarded'],
  'crash.report': ['crash.id', 'crash.process', 'crash.exception_code', 'crash.module', 'crash.dump_bytes'],
} as const satisfies Record<string, readonly AttrName[]>;
export type EventName = keyof typeof EVENTS;
