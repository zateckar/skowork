// Spike for add-agent-created-skills task 1.1: does the model find `skills.save` through `find_tools` and call it
// without asking for a file path? Mirrors the lazy tool loading of client/src/runtime: the first turn offers only
// find_tools and ask_user; skills.save is offered after find_tools has loaded it. Tool results are mocked.
// Prints one line per prompt: calls made, whether a path was asked, and the final text (truncated). Never prints the key.
//   node dev/scripts/skills-save-spike.mjs [model]
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

// The production system prompt (client/src/runtime/types.h) plus the fixed sentence of design D2.
const BASE_PROMPT = 'You are SkoWork, an assistant that works for the user on their computer. Use the tools you have; ' +
  'call find_tools to discover more. Ask the user with ask_user when you need a decision. ' +
  'Delegate independent pieces of work to subagents with delegate. When an answer uses a passage ' +
  'that knowledge.search returned, write that passage\'s number in square brackets after the ' +
  'sentence it supports, such as [2]. Do not write the number of a passage you did not use.';
const D2_SENTENCE = 'A Skill needs no file path. To create or save a Skill, find skills.save with find_tools and call it; ' +
  'the tool stores the Skill for the user. Never ask the user where to save a Skill or which folder to use.';
const SYSTEM = `${BASE_PROMPT}\n\n${D2_SENTENCE}`;

const FIND_TOOLS = {
  type: 'function',
  function: {
    name: 'find_tools',
    description: 'Search the available tools by name and description. The tools found are loaded for the rest of this run.',
    parameters: {
      type: 'object',
      properties: {
        query: { type: 'string', minLength: 1, description: 'What you want to do, for example: calendar events' },
        max_results: { type: 'integer', minimum: 1, maximum: 10 },
      },
      required: ['query'],
    },
  },
};
const ASK_USER = {
  type: 'function',
  function: {
    name: 'ask_user',
    description: 'Ask the user a question and wait for the answer. Use it when you need a decision or missing information.',
    parameters: {
      type: 'object',
      properties: {
        question: { type: 'string', minLength: 1 },
        choices: { type: 'array', items: { type: 'string' }, maxItems: 8 },
      },
      required: ['question'],
    },
  },
};
// Candidate tool description (task 1.1 tests this text; it changes only if the model asks for a path).
const SKILLS_SAVE = {
  type: 'function',
  function: {
    name: 'skills.save',
    description: 'Save a Skill you drafted as one of the user\'s private Skills, so it can be used in later Runs. Give name ' +
      '(kebab-case), description and instructions. A Skill needs no file path: this tool stores it, and the user approves ' +
      'the text first. Set update to true with a higher version to change a Skill you created earlier. Never ask the user ' +
      'where to save a Skill.',
    parameters: {
      type: 'object',
      properties: {
        name: { type: 'string', description: 'Kebab-case Skill name, for example weekly-status-email' },
        description: { type: 'string', description: 'When to use the Skill, one or two sentences' },
        instructions: { type: 'string', description: 'The Skill instructions in Markdown' },
        version: { type: 'string', description: 'Semantic version, default 1.0.0' },
        tools: { type: 'array', items: { type: 'string' }, description: 'Tool ids the Skill declares' },
        risk: { type: 'string', enum: ['read', 'write-local', 'external', 'destructive'] },
        network: { type: 'string', enum: ['none', 'proxy'] },
        files: {
          type: 'array',
          items: { type: 'object', properties: { path: { type: 'string' }, content: { type: 'string' } }, required: ['path', 'content'] },
        },
        update: { type: 'boolean', description: 'True to replace a Skill you created earlier' },
      },
      required: ['name', 'description', 'instructions'],
    },
  },
};

const PROMPTS = [
  'make me a skill that turns my meeting notes into action items with owners and due dates',
  'I want a reusable skill that reviews a pull request description against our review checklist',
  'Create a skill that drafts a weekly status email from my task list.',
  'Can you make a skill for checking that a CSV export has the right columns before I upload it?',
];
const PATH_RE = /\b(where (should|do|would) (i|you)|which (folder|directory|path|location)|file ?name|what (path|folder|location)|save (it )?(to|in|under))\b/i;

async function chat(messages, tools) {
  const body = { model, messages, temperature: 0, max_tokens: 2000, tools };
  const r = await fetch(`${base}/chat/completions`, {
    method: 'POST',
    headers: { 'content-type': 'application/json', authorization: `Bearer ${env.OPENAI_KEY}` },
    body: JSON.stringify(body),
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(`status ${r.status} ${String(j.error?.message ?? '').slice(0, 160)}`);
  return j.choices[0].message;
}

for (const prompt of PROMPTS) {
  const messages = [{ role: 'system', content: SYSTEM }, { role: 'user', content: prompt }];
  let loaded = [FIND_TOOLS, ASK_USER];
  const calls = [];
  let askedPath = false;
  let text = '';
  try {
    for (let turn = 0; turn < 6; turn++) {
      const m = await chat(messages, loaded);
      messages.push(m);
      if (m.content && PATH_RE.test(m.content)) askedPath = true;
      if (!m.tool_calls?.length) { text = m.content ?? ''; break; }
      for (const tc of m.tool_calls) {
        const name = tc.function.name;
        const args = tc.function.arguments;
        calls.push(name);
        let result;
        if (name === 'find_tools') {
          if (!loaded.includes(SKILLS_SAVE)) loaded = [...loaded, SKILLS_SAVE];
          result = 'Found 1 tool, now loaded: skills.save - Save a Skill you drafted as one of the user\'s private Skills.';
        } else if (name === 'skills.save') {
          const a = JSON.parse(args);
          if (a.path !== undefined) askedPath = true;
          result = `Saved private Skill "${a.name}" (created, version ${a.version ?? '1.0.0'}). The user can use it from the next Run.`;
        } else if (name === 'ask_user') {
          if (PATH_RE.test(JSON.parse(args).question ?? '')) askedPath = true;
          result = 'The user did not answer.';
        } else {
          result = `Unknown tool ${name}`;
        }
        messages.push({ role: 'tool', tool_call_id: tc.id, content: result });
      }
    }
  } catch (e) {
    text = `ERROR ${e.message}`;
  }
  const saved = calls.includes('skills.save');
  console.log(
    `calls ${calls.join(',') || '-'}`.padEnd(44),
    `find_tools ${calls.includes('find_tools') ? 'yes' : 'no '}`.padEnd(16),
    `saved ${saved ? 'yes' : 'no '}`.padEnd(12),
    `asked_path ${askedPath ? 'YES' : 'no'}`.padEnd(18),
    `| ${prompt.slice(0, 40)}`,
  );
  console.log(`   text: ${text.replace(/\s+/g, ' ').slice(0, 220)}`);
}
