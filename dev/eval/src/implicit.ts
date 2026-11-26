// Labels taken from the user's own decisions (add-evaluation-export design D5, step 1). They are biased towards items the
// user happened to touch, so they are marked user_decision and reported apart from drafted and reviewed labels.
import type { Json } from './io.ts';
import type { AgendaLabel, FactLabel, Label, MeetingLabel, TriageLabel } from './labels.ts';

interface Decision {
  kind: string;
  result?: string;
  before?: Json;
  after?: Json;
}

function decisionsOf(rec: Json): Decision[] {
  const user = rec.user ?? {};
  const out: Decision[] = Array.isArray(user.decisions) ? [...user.decisions] : [];
  // Decisions made before the log existed: the stored suggestion state.
  const legacy = user.legacy;
  if (legacy && typeof legacy.suggestion === 'string') {
    const kind = { dismissed: 'dismiss', no_action: 'no_action', started: 'start' }[legacy.suggestion as string];
    if (kind && !out.some((d) => d.kind === kind)) out.push({ kind });
  }
  return out;
}

export function implicitLabels(records: Json[]): Label[] {
  const labels: Label[] = [];
  for (const rec of records) {
    const id = String(rec.id);
    const decisions = decisionsOf(rec);
    // Triage: the latest decision wins (a Start work after a Dismiss means the item did need action).
    const triage = decisions.filter((d) => ['dismiss', 'no_action', 'start'].includes(d.kind)).at(-1);
    if (triage) {
      const l: TriageLabel = { step: 'triage', id, source: 'user_decision', action: triage.kind === 'start' ? 'required' : 'info' };
      if (triage.kind === 'start') l.relevant = true;
      labels.push(l);
    }
    // Agenda: done means a right item, dismissed a false one.
    const agendaDone = decisions.filter((d) => d.kind === 'agenda_done');
    const agendaDismissed = decisions.filter((d) => d.kind === 'agenda_dismiss');
    if (agendaDone.length || agendaDismissed.length) {
      const items: Json[] = rec.skowork?.agenda ?? [];
      const kept = items.filter((a) => !agendaDismissed.some((d) => d.result === `agenda:${a.id}`));
      const l: AgendaLabel = {
        step: 'agenda',
        id,
        source: 'user_decision',
        commitments: kept.map((a) => ({ quote: String(a.quote ?? ''), owner: 'me', due: a.due_at ? new Date(a.due_at).toISOString().slice(0, 10) : undefined })),
      };
      labels.push(l);
    }
    // Facts: a deleted fact was wrong or trivial; a fact marked high is important, low is trivial.
    for (const d of decisions) {
      const fact = d.before && (d.before.subject || d.before.text) ? [d.before.subject, d.before.predicate, d.before.object].filter(Boolean).join(' ') : '';
      if (!fact) continue;
      if (d.kind === 'fact_delete') labels.push({ step: 'knowledge', id, source: 'user_decision', fact, keep: 'wrong' } satisfies FactLabel);
      if (d.kind === 'importance' && d.after?.level === 'high') labels.push({ step: 'knowledge', id, source: 'user_decision', fact, keep: 'important' } satisfies FactLabel);
      if (d.kind === 'importance' && d.after?.level === 'low') labels.push({ step: 'knowledge', id, source: 'user_decision', fact, keep: 'trivial' } satisfies FactLabel);
    }
    // Speakers the user named: the cluster belongs to that person. Names the client chose are not labels.
    const meta: Json = rec.item?.speaker_meta ?? {};
    const speakers: Json = rec.item?.speakers ?? {};
    const named: Record<string, string> = {};
    for (const [k, v] of Object.entries(meta)) if ((v as Json).by === 'user' && typeof speakers[k] === 'string') named[k] = speakers[k];
    if (Object.keys(named).length) labels.push({ step: 'meeting', id, source: 'user_decision', speakers: named } satisfies MeetingLabel);
  }
  return labels;
}
