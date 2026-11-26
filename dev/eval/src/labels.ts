// Labels beside an evaluation export (add-evaluation-export design D5): one per item and step in labels.jsonl. Every
// label says who set it: the user's own decision, a draft by the developer's assistant, or the user.

export type LabelSource = 'user_decision' | 'drafted' | 'user';
export type Action = 'required' | 'info' | 'none';
export type Urgency = 'now' | 'this_week' | 'later' | 'stale';

export interface TriageLabel {
  step: 'triage';
  id: string;
  source: LabelSource;
  relevant?: boolean;
  action?: Action;
  category?: string;
  urgency?: Urgency;
  unsure?: boolean;
  why?: string;
}

export interface Commitment {
  quote: string;
  owner: string;
  due?: string;
}

export interface AgendaLabel {
  step: 'agenda';
  id: string;
  source: LabelSource;
  commitments: Commitment[];
  why?: string;
}

export interface FactLabel {
  step: 'knowledge';
  id: string; // the item
  source: LabelSource;
  fact: string; // a statement or "subject predicate object"
  keep: 'important' | 'ok' | 'trivial' | 'wrong';
}

export interface Turn {
  start_ms: number;
  end_ms: number;
  speaker: string; // a cluster id in replays, a person in references
}

export interface MeetingLabel {
  step: 'meeting';
  id: string;
  source: LabelSource;
  // cluster id -> person (an attendee address or name), or "unknown"
  speakers?: Record<string, string>;
  // corrected reference minutes
  reference?: { start_ms: number; end_ms: number; text: string; turns: Turn[] }[];
  terms?: string[];
}

export type Label = TriageLabel | AgendaLabel | FactLabel | MeetingLabel;

export function isLabel(x: unknown): x is Label {
  if (typeof x !== 'object' || x === null) return false;
  const o = x as Record<string, unknown>;
  return typeof o.id === 'string' && typeof o.source === 'string' && ['triage', 'agenda', 'knowledge', 'meeting'].includes(String(o.step));
}
