// control-plane/api-connectors Document Parsing Safety, API Document Upload and Tool Schema Mapping (design D2, D7, D9).
// Reads one OpenAPI 3.0.x or 3.1.x document (JSON or YAML) with limits, refuses external references, and resolves
// local references up to depth 8. It never fetches anything.
import { parse as parseYaml } from 'yaml';
import { ApiError } from '../http.ts';

export const MAX_REF_DEPTH = 8;
export const MAX_OPERATIONS = 300;
const MAX_NODES = 200_000;
const MAX_NESTING = 64;
const MAX_MAPPED_NODES = 50_000;
const METHODS = ['get', 'put', 'post', 'patch', 'delete', 'head', 'options', 'trace'] as const;

export type Obj = Record<string, unknown>;

export interface RawParameter {
  name: string;
  in: string;
  required: boolean;
  description: string;
  schema: unknown;
}

export interface RawOperation {
  method: string;
  path: string;
  operationId?: string;
  description: string;
  parameters: RawParameter[];
  requestBody?: { required: boolean; json: boolean; schema: unknown };
  /** Set when the operation cannot be mapped at all (a reference that cannot be resolved, a schema too deep). */
  mappingError?: string;
}

export interface ParsedDocument {
  openapi: string;
  title: string;
  description: string;
  operations: RawOperation[];
  /** Resolves a local reference of the document (used for the report and tests). */
  resolve(ref: string): unknown;
  /** Maps a schema to a plain object with its references resolved, or throws a MappingError. */
  inline(schema: unknown): unknown;
}

export class MappingError extends Error {}

const invalid = (message: string, field?: string) => new ApiError(400, 'VALIDATION_FAILED', message, field ? { field } : {});

/** Reads the text as JSON or YAML, with limits on size, nesting, aliases and expanded size. */
export function readDocument(text: string, maxBytes: number): Obj {
  if (Buffer.byteLength(text, 'utf8') > maxBytes) {
    throw new ApiError(413, 'TOO_LARGE', `The document is larger than the limit of ${maxBytes} bytes.`);
  }
  let doc: unknown;
  try {
    doc = text.trimStart().startsWith('{') ? JSON.parse(text) : parseYaml(text, { maxAliasCount: 50, uniqueKeys: true });
  } catch {
    throw invalid('The document is neither valid JSON nor valid YAML.');
  }
  // Walk the expanded structure with a budget: aliases share objects, so the budget bounds time as well as size.
  let nodes = 0;
  const walk = (v: unknown, depth: number): void => {
    if (depth > MAX_NESTING) throw invalid(`The document is nested more than ${MAX_NESTING} levels deep.`);
    if (++nodes > MAX_NODES) throw invalid('The document expands to too many nodes.');
    if (Array.isArray(v)) for (const x of v) walk(x, depth + 1);
    else if (v && typeof v === 'object') for (const x of Object.values(v)) walk(x, depth + 1);
  };
  walk(doc, 0);
  if (!doc || typeof doc !== 'object' || Array.isArray(doc)) throw invalid('The document must be an object.');
  return doc as Obj;
}

function findExternalRef(v: unknown): string | undefined {
  if (Array.isArray(v)) {
    for (const x of v) {
      const f = findExternalRef(x);
      if (f) return f;
    }
    return undefined;
  }
  if (!v || typeof v !== 'object') return undefined;
  for (const [k, x] of Object.entries(v as Obj)) {
    if (k === '$ref' && typeof x === 'string' && !x.startsWith('#/') && x !== '#') return x;
    const f = findExternalRef(x);
    if (f) return f;
  }
  return undefined;
}

/** Parses the document: version, external references, title and description, and every operation. */
export function parseOpenApi(doc: Obj): ParsedDocument {
  if (doc.swagger !== undefined) {
    throw invalid('Swagger 2.0 is not supported. Upload an OpenAPI 3.0 or OpenAPI 3.1 document.');
  }
  const version = doc.openapi;
  if (typeof version !== 'string' || !/^3\.[01]\.\d+$/.test(version)) {
    throw invalid('The document must declare OpenAPI 3.0.x or 3.1.x in its openapi field.', 'openapi');
  }
  const ext = findExternalRef(doc);
  if (ext) throw invalid(`The document references "${ext}" outside itself. External references are refused.`, '$ref');

  const resolve = (ref: string): unknown => {
    if (!ref.startsWith('#/') && ref !== '#') throw new MappingError(`the reference ${ref} cannot be resolved`);
    if (ref === '#') return doc;
    let cur: unknown = doc;
    for (const raw of ref.slice(2).split('/')) {
      const part = decodeURIComponent(raw).replace(/~1/g, '/').replace(/~0/g, '~');
      if (!cur || typeof cur !== 'object' || !(part in (cur as Obj))) throw new MappingError(`the reference ${ref} cannot be resolved`);
      cur = (cur as Obj)[part];
    }
    return cur;
  };

  // Copies a schema with its references replaced by their targets. refDepth counts the references on one chain.
  const inline = (schema: unknown): unknown => {
    let mapped = 0;
    const copy = (node: unknown, refDepth: number): unknown => {
      if (++mapped > MAX_MAPPED_NODES) throw new MappingError('the schema is too large to map');
      if (Array.isArray(node)) return node.map((n) => copy(n, refDepth));
      if (!node || typeof node !== 'object') return node;
      const o = node as Obj;
      if (typeof o.$ref === 'string') {
        if (refDepth >= MAX_REF_DEPTH) throw new MappingError(`the schema is too deep: more than ${MAX_REF_DEPTH} levels of references`);
        return copy(resolve(o.$ref), refDepth + 1);
      }
      const out: Obj = {};
      for (const [k, x] of Object.entries(o)) out[k] = copy(x, refDepth);
      return out;
    };
    return copy(schema, 0);
  };

  const info = (doc.info ?? {}) as Obj;
  const title = typeof info.title === 'string' ? info.title.trim() : '';
  if (!title) throw invalid('The document has no title in its info object.', 'info.title');
  const description = typeof info.description === 'string' ? info.description : '';

  const operations: RawOperation[] = [];
  const paths = (doc.paths ?? {}) as Obj;
  if (typeof paths !== 'object' || Array.isArray(paths)) throw invalid('paths must be an object.', 'paths');
  for (const [path, rawItem] of Object.entries(paths)) {
    let item = rawItem as Obj;
    if (item && typeof item.$ref === 'string') {
      try {
        item = resolve(item.$ref) as Obj;
      } catch (err) {
        item = {};
        operations.push({ method: 'get', path, description: '', parameters: [], mappingError: (err as Error).message });
        continue;
      }
    }
    if (!item || typeof item !== 'object') continue;
    const shared = Array.isArray(item.parameters) ? (item.parameters as unknown[]) : [];
    for (const method of METHODS) {
      const op = item[method] as Obj | undefined;
      if (!op || typeof op !== 'object') continue;
      const own = Array.isArray(op.parameters) ? (op.parameters as unknown[]) : [];
      const raw: RawOperation = {
        method,
        path,
        operationId: typeof op.operationId === 'string' ? op.operationId : undefined,
        description: typeof op.description === 'string' ? op.description : '',
        parameters: [],
      };
      try {
        // Path-level parameters apply unless the operation declares one with the same name and location.
        const params = new Map<string, RawParameter>();
        for (const p of [...shared, ...own]) {
          const r = p && typeof p === 'object' && typeof (p as Obj).$ref === 'string' ? (resolve((p as Obj).$ref as string) as Obj) : (p as Obj);
          if (!r || typeof r.name !== 'string' || typeof r.in !== 'string') throw new MappingError('a parameter has no name or location');
          params.set(`${r.in}:${r.name}`, {
            name: r.name,
            in: r.in,
            required: r.required === true || r.in === 'path',
            description: typeof r.description === 'string' ? r.description : '',
            schema: r.schema ?? { type: 'string' },
          });
        }
        raw.parameters = [...params.values()];
        if (op.requestBody && typeof op.requestBody === 'object') {
          let body = op.requestBody as Obj;
          if (typeof body.$ref === 'string') body = resolve(body.$ref) as Obj;
          const content = (body.content ?? {}) as Obj;
          const json = Object.keys(content).find((k) => k === 'application/json') ?? Object.keys(content).find((k) => /\+json$/i.test(k));
          const media = json ? (content[json] as Obj) : undefined;
          raw.requestBody = {
            required: body.required === true,
            json: Boolean(json),
            schema: media?.schema ?? { type: 'object' },
          };
          // A request body without any content type carries nothing; one with only other types is not supported.
          if (!json && Object.keys(content).length === 0) delete raw.requestBody;
          else if (!json) raw.requestBody.schema = undefined;
        }
      } catch (err) {
        raw.mappingError = err instanceof MappingError ? err.message : (err as Error).message;
      }
      operations.push(raw);
    }
  }
  return {
    openapi: version,
    title,
    description,
    operations,
    resolve,
    inline,
  };
}
