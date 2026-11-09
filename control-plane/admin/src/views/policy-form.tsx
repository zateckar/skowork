// The structured policy editor (improve-usability: admin-console Structured Policy Editor; design D2): one control per
// policy key, generated from the key registry the policy service serves, editing the same draft as the JSON view.
import { useState } from 'react';
import { useLoad } from '../ui.tsx';
import type { FieldError } from './policy.tsx';

type KeyType =
  | { kind: 'boolean' }
  | { kind: 'integer' | 'number'; min?: number; max?: number }
  | { kind: 'string'; pattern?: string; maxLength?: number }
  | { kind: 'enum'; values: string[] }
  | { kind: 'list'; item: KeyType; maxItems?: number }
  | { kind: 'decision-map'; keys?: string[] }
  | { kind: 'map'; value: KeyType; keys?: string[] };
interface KeyDef { path: string; owner: string; description?: string; type: KeyType; default?: unknown; merge: string; guardrail?: string }
interface GeneratedField { path: string; owner: string; description: string; editable: false }
interface ToolDetail { description: string; owner: string; declared_risk: string; prerequisite: string; caveat: string; decision_paths?: string[] }

type Doc = Record<string, any>;
const DECISIONS = ['allow', 'approve', 'deny'];
const MERGE_HELP: Record<string, string> = {
  scalar: 'The highest-priority group that sets this value decides.',
  'whole-map': 'The highest-priority group supplies the entire map.',
  union: 'Entries from groups are combined.',
  'decision-map': 'Each entry is decided separately; any group deny wins, otherwise group priority decides.',
  'per-key-map': 'Each map entry comes from the highest-priority group that sets it.',
};
const GUARDRAIL_HELP: Record<string, string> = {
  add: 'The guardrail adds entries.', cap: 'The guardrail sets an upper bound.', floor: 'The guardrail sets a lower bound.',
  'max-level': 'The guardrail limits the permitted level.', restrict: 'The guardrail restricts the allowed entries.',
  force: 'The guardrail sets this value.', 'force-entries': 'The guardrail sets the named map entries.',
  stricter: 'The guardrail can make matching decisions stricter.',
};

export function getPath(doc: Doc, path: string): unknown {
  return path.split('.').reduce<any>((o, k) => (o && typeof o === 'object' ? o[k] : undefined), doc);
}

/** A copy of `doc` with `path` set to `value`; undefined removes the key and any objects left empty. */
export function setPath(doc: Doc, path: string, value: unknown): Doc {
  const next = structuredClone(doc);
  const parts = path.split('.');
  const chain: Doc[] = [next];
  let node = next;
  for (const p of parts.slice(0, -1)) {
    if (typeof node[p] !== 'object' || node[p] === null || Array.isArray(node[p])) {
      if (value === undefined) return next;
      node[p] = {};
    }
    node = node[p];
    chain.push(node);
  }
  if (value === undefined) delete node[parts.at(-1)!];
  else node[parts.at(-1)!] = value;
  for (let i = chain.length - 1; i > 0; i--) if (!Object.keys(chain[i]!).length) delete chain[i - 1]![parts[i - 1]!];
  return next;
}

const show = (v: unknown) => (v === undefined ? '' : typeof v === 'string' ? v : JSON.stringify(v));
const range = (t: KeyType) =>
  t.kind === 'integer' || t.kind === 'number'
    ? [t.min !== undefined && `≥ ${t.min}`, t.max !== undefined && `≤ ${t.max}`].filter(Boolean).join(', ')
    : t.kind === 'string' && t.pattern ? `pattern ${t.pattern}` : '';

export function PolicyForm({ doc, onChange, errors, guardrail }: { doc: Doc; onChange: (d: Doc) => void; errors: FieldError[]; guardrail: boolean }) {
  const reg = useLoad<{ keys: KeyDef[]; generated_fields?: GeneratedField[]; built_in_tools?: string[]; discovered_tools?: string[]; tool_details?: Record<string, ToolDetail> }>('/api/v1/admin/policy/schema?keys=true');
  const [filter, setFilter] = useState('');
  if (!reg.data) return <p className="muted">{reg.error || 'Loading the policy keys…'}</p>;
  const query = filter.trim().toLowerCase();
  const tools = [...(reg.data.built_in_tools ?? []), ...(reg.data.discovered_tools ?? [])];
  const details = reg.data.tool_details ?? {};
  const matchingTools = query ? tools.filter((id) => `${id} ${details[id]?.description ?? ''}`.toLowerCase().includes(query)) : [];
  const matchesTool = matchingTools.length > 0;
  const generated = (reg.data.generated_fields ?? []).filter((field) => !query || `${field.path} ${field.owner} ${field.description}`.toLowerCase().includes(query));
  const keys = reg.data.keys.filter((k) => {
    const value = getPath(doc, k.path);
    const ruleMap = k.path === 'tools.rules' || k.path === 'unattended.rules';
    const matchesPattern = ruleMap && value && typeof value === 'object' && !Array.isArray(value) &&
      Object.keys(value).some((pattern) => pattern.toLowerCase().includes(query));
    return (!guardrail || k.guardrail) && (!query || `${k.path} ${k.owner} ${k.description ?? ''}`.toLowerCase().includes(query) || (ruleMap && matchesTool) || matchesPattern);
  });
  const sections = new Map<string, KeyDef[]>();
  for (const k of [...keys].sort((a, b) => a.path.localeCompare(b.path))) {
    const top = k.path.split('.')[0]!;
    sections.set(top, [...(sections.get(top) ?? []), k]);
  }
  const errorOf = (path: string) =>
    errors.filter((e) => { const p = e.path ?? e.keys?.[0] ?? ''; return p === path || p.startsWith(`${path}.`) || p.startsWith(`${path}[`); }).map((e) => e.message).join('; ');
  return (
    <div className="policy-form" data-testid="policy-form">
      <div className="row">
        <label className="field grow"><span>Find a key</span><input data-testid="policy-form-filter" value={filter} onChange={(e) => setFilter(e.target.value)} placeholder="for example computer" /></label>
      </div>
      {[...sections].map(([top, defs]) => {
        const set = defs.filter((d) => getPath(doc, d.path) !== undefined).length;
        const bad = defs.some((d) => errorOf(d.path));
        return (
          <details key={top} open={!!filter || set > 0 || bad} data-testid={`policy-section-${top}`}>
            <summary><b>{top}</b> <small>{set ? `${set} set` : 'all inherited'}{bad ? ' · has errors' : ''}</small></summary>
            {defs.map((d) => (
              <KeyRow key={d.path} def={d} value={getPath(doc, d.path)} error={errorOf(d.path)} tools={tools} details={details} matchingTools={matchingTools} onChange={(v) => onChange(setPath(doc, d.path, v))} />
            ))}
          </details>
        );
      })}
      {generated.length > 0 && <details open={!!filter} data-testid="policy-generated-fields">
        <summary><b>Generated bundle fields</b> <small>managed outside policy drafts</small></summary>
        {generated.map((field) => <div className="key-row" key={field.path} data-testid={`generated-${field.path}`}>
          <div className="key-label"><code>policy.{field.path}</code><small>{field.owner}</small></div>
          <div className="key-control">{field.description}</div>
        </div>)}
      </details>}
      {!sections.size && !generated.length && <p className="empty">No key matches.</p>}
    </div>
  );
}

function KeyRow({ def, value, error, tools, details, matchingTools, onChange }: { def: KeyDef; value: unknown; error: string; tools: string[]; details: Record<string, ToolDetail>; matchingTools: string[]; onChange: (v: unknown) => void }) {
  const id = `key-${def.path}`;
  const inherited = value === undefined;
  return (
    <div className={`key-row${error ? ' bad' : ''}${inherited ? '' : ' set'}`} data-testid={`key-row-${def.path}`}>
      <div className="key-label">
        <label htmlFor={id}><code>policy.{def.path}</code></label>
        {def.description && <small>{def.description}</small>}
        <small>{def.owner}{range(def.type) ? ` · ${range(def.type)}` : ''}{def.default !== undefined ? ` · default ${show(def.default)}` : ''}</small>
        <small>{MERGE_HELP[def.merge] ?? `Group merge: ${def.merge}.`} {def.guardrail ? (GUARDRAIL_HELP[def.guardrail] ?? `Guardrail: ${def.guardrail}.`) : 'This setting has no guardrail mode.'}</small>
      </div>
      <div className="key-control">
        <Control id={id} def={def} value={value} tools={tools} details={details} matchingTools={matchingTools} onChange={onChange} />
        {!inherited && <button type="button" className="icon" aria-label={`Reset policy.${def.path} to inherited`} title="Reset to inherited" onClick={() => onChange(undefined)}>↺</button>}
      </div>
      {error && <p className="err" data-testid={`form-error-${def.path}`}>{error}</p>}
    </div>
  );
}

function Control({ id, def, value, tools, details, matchingTools, onChange }: { id: string; def: KeyDef; value: unknown; tools: string[]; details: Record<string, ToolDetail>; matchingTools: string[]; onChange: (v: unknown) => void }) {
  const t = def.type;
  switch (t.kind) {
    case 'boolean':
      return (
        <select id={id} data-testid={id} value={value === undefined ? '' : String(value)} onChange={(e) => onChange(e.target.value === '' ? undefined : e.target.value === 'true')}>
          <option value="">inherited</option><option value="true">true</option><option value="false">false</option>
        </select>
      );
    case 'enum':
      return (
        <select id={id} data-testid={id} value={typeof value === 'string' ? value : ''} onChange={(e) => onChange(e.target.value || undefined)}>
          <option value="">inherited</option>
          {t.values.map((v) => <option key={v}>{v}</option>)}
        </select>
      );
    case 'integer':
    case 'number':
      return (
        <input id={id} data-testid={id} type="number" min={t.min} max={t.max} step={t.kind === 'integer' ? 1 : 'any'} placeholder="inherited"
          value={typeof value === 'number' ? value : ''} onChange={(e) => onChange(e.target.value === '' ? undefined : Number(e.target.value))} />
      );
    case 'string':
      return <input id={id} data-testid={id} placeholder="inherited" value={typeof value === 'string' ? value : ''} onChange={(e) => onChange(e.target.value === '' ? undefined : e.target.value)} />;
    case 'list':
      if (t.item.kind === 'enum') {
        const chosen = Array.isArray(value) ? (value as string[]) : null;
        return (
          <span className="row" id={id} data-testid={id}>
            {t.item.values.map((v) => (
              <label key={v} className="check">
                <input type="checkbox" checked={chosen?.includes(v) ?? false}
                  onChange={(e) => onChange(e.target.checked ? [...(chosen ?? []), v] : (chosen ?? []).filter((x) => x !== v))} /> {v}
              </label>
            ))}
            {!chosen && <small>inherited</small>}
          </span>
        );
      }
      return <ListText id={id} value={value} onChange={onChange} />;
    case 'decision-map':
      return <MapTable id={id} value={value} fixed={t.keys} values={DECISIONS}
        suggestions={def.path === 'tools.rules' || def.path === 'unattended.rules' ? tools : undefined} details={details} matches={matchingTools} onChange={onChange} />;
    case 'map':
      return <MapTable id={id} value={value} fixed={t.keys} values={t.value.kind === 'enum' ? t.value.values : undefined} onChange={onChange} />;
  }
}

/** A list of strings, one per line; the text is kept while typing so blank lines do not jump away. */
function ListText({ id, value, onChange }: { id: string; value: unknown; onChange: (v: unknown) => void }) {
  const fromValue = Array.isArray(value) ? (value as unknown[]).map(String).join('\n') : '';
  const [text, setText] = useState(fromValue);
  const shown = text.split('\n').map((s) => s.trim()).filter(Boolean).join('\n') === fromValue ? text : fromValue;
  return (
    <textarea id={id} data-testid={id} rows={Math.min(8, Math.max(2, shown.split('\n').length + 1))} placeholder="inherited (one entry per line)" value={shown}
      onChange={(e) => {
        setText(e.target.value);
        const items = e.target.value.split('\n').map((s) => s.trim()).filter(Boolean);
        onChange(items.length ? items : e.target.value === '' ? undefined : []);
      }} />
  );
}

/** A map edited as rows of key and value; maps with fixed keys show one row per key. */
function MapTable({ id, value, fixed, values, suggestions, details, matches, onChange }: { id: string; value: unknown; fixed?: string[]; values?: string[]; suggestions?: string[]; details?: Record<string, ToolDetail>; matches?: string[]; onChange: (v: unknown) => void }) {
  const map = value && typeof value === 'object' && !Array.isArray(value) ? (value as Record<string, unknown>) : null;
  const [newKey, setNewKey] = useState('');
  const [newDecision, setNewDecision] = useState('');
  const put = (k: string, v: unknown) => {
    const next = { ...(map ?? {}) };
    if (v === undefined || (fixed && v === '')) delete next[k];
    else next[k] = v;
    onChange(Object.keys(next).length ? next : undefined);
  };
  const cell = (k: string) =>
    values ? (
      <select aria-label={`${k} value`} data-testid={`${id}-${k}`} value={show(map?.[k])} onChange={(e) => put(k, e.target.value || undefined)}>
        <option value="">{fixed || !map?.[k] ? 'inherited' : '(remove)'}</option>
        {values.map((v) => <option key={v}>{v}</option>)}
      </select>
    ) : (
      <input aria-label={`${k} value`} data-testid={`${id}-${k}`} placeholder={fixed ? 'inherited' : ''} value={show(map?.[k])} onChange={(e) => put(k, e.target.value)} />
    );
  const rows = fixed ?? [...new Set([...Object.keys(map ?? {}), ...(suggestions ? matches ?? [] : [])])];
  return (
    <div className="map-table" id={id} data-testid={id}>
      {rows.map((k) => (
        <div key={k}>
          <div className="row"><code className="map-key">{k}</code>{cell(k)}
            {!fixed && map?.[k] !== undefined && <button type="button" className="icon" aria-label={`Remove ${k}`} title="Remove" onClick={() => put(k, undefined)}>×</button>}
          </div>
          {details?.[k] && <small>{details[k].description} Owner: {details[k].owner}. Declared risk: {details[k].declared_risk}. {details[k].prerequisite} {details[k].caveat} {details[k].decision_paths?.map((path) => `policy.${path}`).join(' · ')}</small>}
        </div>
      ))}
      {!fixed && (
        <div className="row">
          <input aria-label="New entry" data-testid={`${id}-new`} list={suggestions ? `${id}-suggestions` : undefined} placeholder={values === DECISIONS ? 'tool pattern, for example computer.*' : 'key'} value={newKey} onChange={(e) => setNewKey(e.target.value)} />
          {suggestions && <datalist id={`${id}-suggestions`}>{suggestions.map((tool) => <option key={tool} value={tool} />)}</datalist>}
          {suggestions && <select aria-label="Decision for new rule" data-testid={`${id}-new-decision`} value={newDecision} onChange={(e) => setNewDecision(e.target.value)}>
            <option value="">Choose a decision</option>{values?.map((v) => <option key={v}>{v}</option>)}
          </select>}
          <button type="button" data-testid={`${id}-add`} disabled={!newKey.trim() || Object.hasOwn(map ?? {}, newKey.trim()) || (!!suggestions && !newDecision)} onClick={() => {
            onChange({ ...(map ?? {}), [newKey.trim()]: suggestions ? newDecision : values?.[0] ?? '' });
            setNewKey('');
            setNewDecision('');
          }}>Add</button>
        </div>
      )}
    </div>
  );
}
