// control-plane/api-connectors Description Rules, Operation Usability, Parameter And Body Mapping, Tool Schema Mapping,
// Tool Risk Classes (design D3, D7, D8, D9). Pure functions: they decide what an upload means, nothing is stored here.
import { type ParsedDocument, type RawOperation, MAX_OPERATIONS, MappingError } from './openapi.ts';

export const RISK_OF_METHOD: Record<string, 'read' | 'external' | 'destructive'> = {
  get: 'read',
  post: 'external',
  put: 'external',
  patch: 'external',
  delete: 'destructive',
};
export const USABLE_METHODS = Object.keys(RISK_OF_METHOD);
export const MAX_SCHEMA_BYTES = 16 * 1024;
export const MAX_SELECTED = 100;

const PLACEHOLDERS = new Set(['todo', 'tbd', 'string', 'description', 'n/a']);
const PARAM_NAME = /^[A-Za-z0-9._-]{1,64}$/;
const TOOL_NAME = /^[A-Za-z0-9_-]{1,64}$/;

/** Text as the rules see it: whitespace collapsed and trimmed. */
export const collapse = (s: string): string => s.replace(/\s+/g, ' ').trim();

/** The reason a description fails the rules, or null when it passes (Description Rules). */
export function descriptionProblem(text: string, bounds: { min: number; max: number }, avoid: { text: string; label: string }[]): string | null {
  const t = collapse(text);
  const len = [...t].length;
  const lower = t.toLowerCase();
  if (PLACEHOLDERS.has(lower)) return 'its description is a placeholder';
  for (const a of avoid) if (a.text && lower === a.text.trim().toLowerCase()) return `its description only repeats the ${a.label}`;
  if (len < bounds.min) return `its description is too short (${len} characters; ${bounds.min} to ${bounds.max} are needed)`;
  if (len > bounds.max) return `its description is too long (${len} characters; at most ${bounds.max} are allowed)`;
  return null;
}

/** The API description must be its own text (Description Rules: API bounds 80 to 2,000 characters). */
export function apiDescriptionProblem(description: string, title: string): string | null {
  const reason = descriptionProblem(description, { min: 80, max: 2000 }, [{ text: title, label: 'title' }]);
  return reason ? `The API needs its own description of 80 to 2,000 characters: ${reason}.` : null;
}

/** The tool name: the operationId when it fits, otherwise method and path with other characters replaced (D7). */
export function toolNameFor(op: Pick<RawOperation, 'method' | 'path' | 'operationId'>): string {
  if (op.operationId && TOOL_NAME.test(op.operationId)) return op.operationId;
  return `${op.method}_${op.path}`.replace(/[^A-Za-z0-9_-]/g, '_');
}

export interface OperationReport {
  key: string;
  method: string;
  path: string;
  description: string;
  tool: string | null;
  usable: boolean;
  reason: string | null;
  risk: 'read' | 'external' | 'destructive' | null;
  /** The input schema of a usable operation (one JSON object). */
  inputSchema: Record<string, unknown> | null;
  schemaBytes: number;
  callSpec: CallSpec | null;
}

/** What the control plane needs to build the HTTP request of a call. */
export interface CallSpec {
  method: string;
  path: string;
  pathParams: string[];
  queryParams: string[];
  body: boolean;
}

export const operationKey = (method: string, path: string): string => `${method.toUpperCase()} ${path}`;

/** Checks every operation and maps the usable ones (Operation Usability, Parameter And Body Mapping, Tool Schema Mapping). */
export function analyzeDocument(doc: ParsedDocument): { operations: OperationReport[]; problem: string | null } {
  if (doc.operations.length > MAX_OPERATIONS) {
    return { operations: [], problem: `The document has ${doc.operations.length} operations; the limit is ${MAX_OPERATIONS}.` };
  }
  const reports = doc.operations.map((op) => mapOperation(doc, op));
  // Duplicate tool names make every operation that shares the name unusable until the document changes (D7).
  const count = new Map<string, number>();
  for (const r of reports) if (r.tool) count.set(r.tool, (count.get(r.tool) ?? 0) + 1);
  for (const r of reports) {
    if (r.tool && (count.get(r.tool) ?? 0) > 1 && r.usable) {
      r.usable = false;
      r.reason = `the tool name "${r.tool}" is used by more than one operation`;
      r.inputSchema = null;
      r.callSpec = null;
    }
  }
  return { operations: reports, problem: null };
}

function mapOperation(doc: ParsedDocument, op: RawOperation): OperationReport {
  const key = operationKey(op.method, op.path);
  const base: OperationReport = {
    key,
    method: op.method.toUpperCase(),
    path: op.path,
    description: collapse(op.description),
    tool: op.operationId && TOOL_NAME.test(op.operationId) ? op.operationId : toolNameFor(op),
    usable: false,
    reason: null,
    risk: RISK_OF_METHOD[op.method] ?? null,
    inputSchema: null,
    schemaBytes: 0,
    callSpec: null,
  };
  const fail = (reason: string) => ({ ...base, usable: false, reason });
  if (!USABLE_METHODS.includes(op.method)) return fail(`the method ${op.method.toUpperCase()} is not supported`);
  if (op.mappingError) return fail(op.mappingError);
  if (!op.path.startsWith('/')) return fail('the path must begin with /');
  if (base.tool && base.tool.length > 64) return fail('the tool name is longer than 64 characters');
  const bad = descriptionProblem(op.description, { min: 40, max: 1000 }, [
    { text: doc.title, label: 'title' },
    { text: op.path, label: 'path' },
    { text: key, label: 'method and path' },
  ]);
  if (bad) return fail(bad);

  try {
    const properties: Record<string, unknown> = {};
    const required: string[] = [];
    const pathParams: string[] = [];
    const queryParams: string[] = [];
    const templated = [...op.path.matchAll(/\{([^}]+)\}/g)].map((m) => m[1]!);
    for (const p of op.parameters) {
      if (p.in !== 'path' && p.in !== 'query') continue; // header and cookie parameters are never sent (D7)
      if (!PARAM_NAME.test(p.name)) return fail(`the parameter name "${p.name}" is not supported (1 to 64 letters, digits, dots, underscores or hyphens)`);
      if (p.name === 'body') return fail('a parameter named body is not allowed');
      if (properties[p.name]) return fail(`the parameter name "${p.name}" is used twice`);
      if (p.in === 'path' && !templated.includes(p.name)) return fail(`the path parameter "${p.name}" is not in the path`);
      properties[p.name] = withDescription(doc.inline(p.schema), p.description);
      if (p.required) required.push(p.name);
      (p.in === 'path' ? pathParams : queryParams).push(p.name);
    }
    for (const t of templated) if (!pathParams.includes(t)) return fail(`the path parameter "${t}" is not declared`);
    let body = false;
    if (op.requestBody) {
      if (!op.requestBody.json) return fail('only JSON request bodies are supported');
      body = true;
      properties.body = doc.inline(op.requestBody.schema);
      if (op.requestBody.required) required.push('body');
    }
    const schema: Record<string, unknown> = { type: 'object', properties, additionalProperties: false };
    if (required.length) schema.required = required;
    const size = Buffer.byteLength(JSON.stringify(schema), 'utf8');
    if (size > MAX_SCHEMA_BYTES) return fail(`the input schema is larger than 16 KB (${size} bytes)`);
    return {
      ...base,
      usable: true,
      inputSchema: schema,
      schemaBytes: size,
      callSpec: { method: op.method, path: op.path, pathParams, queryParams, body },
    };
  } catch (err) {
    if (err instanceof MappingError) return fail(err.message);
    throw err;
  }
}

function withDescription(schema: unknown, description: string): unknown {
  if (!description || !schema || typeof schema !== 'object' || Array.isArray(schema)) return schema;
  return { ...(schema as Record<string, unknown>), description: collapse(description).slice(0, 500) };
}
