// Spike for fix-feed-live-updates task 1.3: does the organization LLM API cite passages with [n] when the stable system
// prompt asks for it? Ten knowledge questions, each with five numbered passages (one relevant, four distractors), answered
// with the citation sentence of client/agent-runtime (types.h). Prints counts only, plus the markers each answer used.
//   node dev/scripts/llm-citation-spike.mjs [model]
import { readFileSync } from 'node:fs';

const env = Object.fromEntries(
  readFileSync(new URL('../../.env', import.meta.url), 'utf8')
    .split(/\r?\n/)
    .map((l) => l.match(/^\s*([A-Za-z_]\w*)\s*=\s*(.*)$/))
    .filter(Boolean)
    .map((m) => [m[1], m[2].trim().replace(/^["']|["']$/g, '')]),
);
const base = env.OPENAI_BASE_URL.replace(/\/+$/, '');
const model = process.argv[2] ?? env.OPENAI_MODEL;

const SYSTEM =
  'You are SkoWork, an assistant that works for the user on their computer. Use the tools you have; call find_tools to discover more. ' +
  'Ask the user with ask_user when you need a decision. Delegate independent pieces of work to subagents with delegate. ' +
  "When an answer uses a passage that knowledge.search returned, write that passage's number in square brackets after the sentence it supports, such as [2]. " +
  'Do not write the number of a passage you did not use.';

// Each question: the relevant passage is at `answer` (1-based); the others are distractors.
const questions = [
  { q: 'What is the budget for the Lisbon offsite?', answer: 2, passages: ['Mail from Petra about the Berlin workshop catering, Tuesday.', 'Budget memo: the Lisbon offsite budget is 48 000 CZK, approved on 3 March.', 'Parking rules for the office garage changed in May.', 'Notes from the sprint review: the mobile release slipped one week.', 'Holiday calendar for the Prague office, 2027.'] },
  { q: 'Who leads the Phoenix project?', answer: 4, passages: ['Minutes: the Atlas migration plan was accepted.', 'Recipe notes for the team lunch in April.', 'Vendor invoice from the printing service, unpaid.', 'Phoenix project charter: Jana Novakova leads the project from 1 October.', 'Office Wi-Fi password rotation schedule.'] },
  { q: 'When does the code freeze start for the 4.2 release?', answer: 1, passages: ['Release 4.2 schedule: code freeze starts on 14 November, release on 28 November.', 'Team photo booking for December.', 'The coffee machine on floor 3 needs descaling.', 'Customer survey results for the Q2 pilot.', 'Expense policy: receipts within 30 days.'] },
  { q: 'Which airline did the Madrid trip use?', answer: 3, passages: ['Hotel Madrid Centro: check-in after 15:00.', 'Train tickets for the Vienna visit are in the shared folder.', 'Flight confirmation: Iberia IB 3412 to Madrid on 9 September, returning 11 September.', 'Gym membership renewal notice.', 'Quarterly newsletter draft.'] },
  { q: 'What is the new password policy minimum length?', answer: 5, passages: ['Onboarding checklist for new laptops.', 'Support hours change to 08:00 to 16:00 from January.', 'Meeting room booking rules.', 'Office cleaning schedule.', 'Security policy v3: passwords must be at least 14 characters, with a password manager recommended.'] },
  { q: 'Where is the archive of the 2025 contracts stored?', answer: 2, passages: ['Contract template for vendors, version 6.', 'Contracts from 2025 are archived in the legal share under /Archive/2025/Contracts.', 'Legal holiday schedule.', 'Draft clause library for NDAs.', 'IT asset register export.'] },
  { q: 'How many days of parental leave does the policy give?', answer: 1, passages: ['Parental leave policy: 20 working days paid leave for each parent, plus unpaid leave by agreement.', 'Lunch voucher amounts for 2026.', 'Travel policy for trains and hotels.', 'Team offsite agenda draft.', 'Wellbeing workshop sign-up sheet.'] },
  { q: 'What did the customer Acme ask to change in the invoice?', answer: 4, passages: ['Product roadmap, third quarter.', 'Greeting card ideas for the anniversary.', 'Conference talk proposal deadline.', 'Acme email: please put the purchase order number on every invoice line, not only on the header.', 'Printer toner order history.'] },
  { q: 'What time does the warehouse open on Saturdays?', answer: 3, passages: ['Delivery windows for the Brno depot in summer.', 'Forklift safety refresher course dates.', 'Warehouse opening hours: Monday to Friday 06:00 to 18:00, Saturdays 07:00 to 12:00.', 'Stock count results for March.', 'Shift swap form.'] },
  { q: 'Which database engine does the knowledge store use?', answer: 2, passages: ['Backup retention: daily for 14 days, weekly for 8 weeks.', 'The knowledge store keeps its index in SQLite with the SQLite3 Multiple Ciphers encryption.', 'Deployment checklist for the control plane.', 'Glossary of product terms.', 'Monitoring dashboard thresholds.'] },
];

const markersOf = (text) => [...text.matchAll(/\[(\d+)\]/g)].map((m) => Number(m[1]));

let withMarkers = 0, unusedMarkers = 0, failed = 0;
for (const [i, item] of questions.entries()) {
  const context = item.passages.map((p, n) => `[${n + 1}] ${p}`).join('\n');
  const body = {
    model,
    temperature: 0,
    max_tokens: 4000,
    messages: [
      { role: 'system', content: SYSTEM },
      { role: 'user', content: `knowledge.search results:\n${context}\n\nQuestion: ${item.q}` },
    ],
  };
  const r = await fetch(`${base}/chat/completions`, {
    method: 'POST',
    headers: { 'content-type': 'application/json', authorization: `Bearer ${env.OPENAI_KEY}` },
    body: JSON.stringify(body),
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) {
    failed++;
    console.log(`Q${i + 1} status ${r.status}`);
    continue;
  }
  const text = j.choices?.[0]?.message?.content ?? '';
  const markers = markersOf(text);
  const unused = markers.filter((n) => n !== item.answer);
  if (markers.length) withMarkers++;
  unusedMarkers += unused.length;
  console.log(`Q${i + 1} markers [${markers.join(', ')}] relevant ${item.answer} unused-marker ${unused.length}`);
}
const answered = questions.length - failed;
console.log(`answers ${answered}/${questions.length}; with at least one marker ${withMarkers}/${answered}; markers on unused passages ${unusedMarkers}`);
