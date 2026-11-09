// Policy-level simulation of the desktop's portable decision function. Device availability,
// concrete resources, target classification and Skill floors remain conditional on the client.
import { entryPath } from './merge.ts';
import { isPlainObject, type Decision } from './schema.ts';

export type ToolRisk = 'read' | 'write-local' | 'external' | 'destructive';
export interface SimulatedCall { tool: string; risk: ToolRisk; unattended: boolean }
export interface DecisionStep {
  stage: string; decision: Decision; rule: string; source: string; reason: string;
  considered?: { decision: Decision; rule: string; source: string; reason: string };
}
export interface ToolDecisionSimulation extends SimulatedCall, DecisionStep {
  steps: DecisionStep[];
  conditions: string[];
}

const severity: Record<Decision, number> = { allow: 0, approve: 1, deny: 2 };
const riskDefaults: Record<ToolRisk, Decision> = { read: 'allow', 'write-local': 'approve', external: 'approve', destructive: 'deny' };
const computerFloors = new Set(['computer.click', 'computer.type', 'computer.keys', 'computer.scroll', 'computer.drag', 'computer.select']);
const isDecision = (value: unknown): value is Decision => value === 'allow' || value === 'approve' || value === 'deny';

function getPath(doc: unknown, path: string): unknown {
  let current: unknown = doc;
  for (const part of path.split('.')) {
    if (!isPlainObject(current) || !Object.hasOwn(current, part)) return undefined;
    current = current[part];
  }
  return current;
}

// Same ASCII case-folding and separator semantics as client/policy/core.cpp. Dynamic
// programming bounds work for patterns containing many wildcards.
export function matchesToolPattern(pattern: string, name: string): boolean {
  if (Buffer.byteLength(pattern) > 4096 || Buffer.byteLength(name) > 32768) return false;
  const fold = (c: number) => c === 47 ? 92 : c >= 65 && c <= 90 ? c + 32 : c;
  const input = Buffer.from(name, 'utf8');
  const p = Buffer.from(pattern, 'utf8');
  const tokens: { kind: 'literal' | 'any' | 'star' | 'double'; value: number }[] = [];
  for (let i = 0; i < p.length; i++) {
    const c = p[i]!;
    if (c === 42) {
      if (p[i + 1] === 42) {
        while (p[i + 1] === 42) i++;
        tokens.push({ kind: 'double', value: 0 });
      } else tokens.push({ kind: 'star', value: 0 });
    } else if (c === 63) tokens.push({ kind: 'any', value: 0 });
    else tokens.push({ kind: 'literal', value: fold(c) });
  }
  let next = new Uint8Array(input.length + 1);
  let cur = new Uint8Array(input.length + 1);
  next[input.length] = 1;
  for (let i = tokens.length - 1; i >= 0; i--) {
    const token = tokens[i]!;
    for (let j = input.length; j >= 0; j--) {
      const more = j < input.length;
      const c = more ? fold(input[j]!) : 0;
      let match = false;
      if (token.kind === 'literal') match = more && c === token.value && !!next[j + 1];
      else if (token.kind === 'any') match = more && c !== 92 && !!next[j + 1];
      else match = !!next[j] || (more && (token.kind === 'double' || c !== 92) && !!cur[j + 1]);
      cur[j] = match ? 1 : 0;
    }
    [cur, next] = [next, cur];
  }
  return !!next[0];
}

function mostSpecific(value: unknown, tool: string): { pattern: string; decision: Decision } | undefined {
  if (!isPlainObject(value)) return undefined;
  let best: { pattern: string; decision: Decision; specificity: number } | undefined;
  for (const [pattern, decision] of Object.entries(value).sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)) {
    if (!isDecision(decision) || !matchesToolPattern(pattern, tool)) continue;
    const bytes = Buffer.from(pattern, 'utf8');
    let literal = 0, wildcard = false;
    for (const c of bytes) c === 42 || c === 63 ? wildcard = true : literal++;
    const specificity = wildcard ? literal : 1_000_000 + literal;
    if (!best || specificity > best.specificity || (specificity === best.specificity && severity[decision] > severity[best.decision]))
      best = { pattern, decision, specificity };
  }
  return best;
}

export function simulateToolDecision(policy: Record<string, unknown>, sources: Record<string, string>, call: SimulatedCall): ToolDecisionSimulation {
  const source = (rule: string) => {
    const key = rule.split('[')[0]!;
    return sources[rule] ?? sources[key] ?? (getPath(policy, key) === undefined ? 'default' : '');
  };
  const steps: DecisionStep[] = [];
  const conditions = [
    'This result assumes the selected risk; the actual action and target may change it.',
    'Device availability, path and network checks, and Skill-specific restrictions are evaluated on the client.',
  ];
  const add = (step: DecisionStep) => { steps.push(step); return step; };
  const switchedKey = call.tool.startsWith('computer.') ? 'tools.computer.mode' : call.tool.startsWith('shell.') ? 'tools.shell.mode' : undefined;
  if (switchedKey && (getPath(policy, switchedKey) ?? 'off') === 'off') {
    const result = add({ stage: 'feature', decision: 'deny', rule: switchedKey, source: source(switchedKey), reason: 'This tool is switched off by policy.' });
    return { ...call, ...result, steps, conditions };
  }
  const match = mostSpecific(getPath(policy, 'tools.rules'), call.tool);
  const classValue = getPath(policy, 'tools.risk');
  const riskDecision = isPlainObject(classValue) ? classValue[call.risk] : undefined;
  const riskRule = entryPath('tools.risk', call.risk);
  let verdict = match
    ? add({ stage: 'tool rule', decision: match.decision, rule: entryPath('tools.rules', match.pattern), source: source(entryPath('tools.rules', match.pattern)), reason: `Matched tool rule ${match.pattern}.` })
    : add({ stage: 'risk', decision: isDecision(riskDecision) ? riskDecision : riskDefaults[call.risk], rule: riskRule,
      source: isDecision(riskDecision) ? source(riskRule) : 'default', reason: `Decision for ${call.risk} actions.` });
  if (verdict.decision === 'deny') return { ...call, ...verdict, steps, conditions };

  const raise = (stage: string, decision: Decision, rule: string, from: string, reason: string) => {
    const effective = severity[decision] > severity[verdict.decision] ? decision : verdict.decision;
    const step = add({ stage, decision: effective, rule: effective === decision && effective !== verdict.decision ? rule : verdict.rule,
      source: effective === decision && effective !== verdict.decision ? from : verdict.source,
      reason: effective === decision && effective !== verdict.decision ? reason : verdict.reason,
      considered: { decision, rule, source: from, reason } });
    verdict = step;
  };
  const unattended = call.unattended ? mostSpecific(getPath(policy, 'unattended.rules'), call.tool) : undefined;
  const floorPattern = matchesToolPattern('mail.send', call.tool) ? 'mail.send' :
    call.unattended ? [...computerFloors].find((pattern) => matchesToolPattern(pattern, call.tool)) : undefined;
  const floorLifted = call.unattended && unattended?.decision === 'allow';
  if (floorPattern && !floorLifted) raise('tool floor', 'approve', `tool:${floorPattern}`, 'tool', `${call.tool} requires approval.`);
  else if (floorPattern && unattended) add({ ...verdict, stage: 'tool floor', considered: {
    decision: 'approve', rule: `tool:${floorPattern}`, source: 'tool',
    reason: `The approval floor is lifted by the explicit unattended allow rule ${unattended.pattern}.`,
  } });
  if (call.unattended) {
    if (unattended) {
      const rule = entryPath('unattended.rules', unattended.pattern);
      raise('unattended rule', unattended.decision, rule, source(rule), `Matched unattended rule ${unattended.pattern}.`);
    }
    const configured = getPath(policy, 'unattended.allowed_risk');
    const allowed = Array.isArray(configured) ? configured : ['read', 'write-local'];
    if (!allowed.includes(call.risk) && unattended?.decision !== 'allow')
      raise('unattended risk', 'approve', 'unattended.allowed_risk', source('unattended.allowed_risk'), `${call.risk} actions require approval unattended.`);
  }
  return { ...call, ...verdict, steps, conditions };
}
