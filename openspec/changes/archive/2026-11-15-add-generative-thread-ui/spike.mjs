// Local-only, synthetic model compliance experiment. Uses the actual SkoWork gateway with an in-memory test
// control plane. Run from the repository root: node --env-file=.env openspec/changes/add-generative-thread-ui/spike.mjs
// Records synthetic UI descriptions for semantic inspection, never configuration, endpoint addresses or credentials.
import { writeFile } from 'node:fs/promises';
import { performance } from 'node:perf_hooks';
import { startHarness } from '../../../control-plane/test/helpers.ts';
import { KEYS } from '../../../control-plane/src/policy/keys.ts';

const mapping = { main: process.env.OPENAI_MODEL || 'skoda-llm-primary', small: 'skoda-llm-secondary',
  vision: process.env.OPENAI_MODEL || 'skoda-llm-primary', embed: 'skoda-llm-embedding',
  rerank: 'skoda-llm-reranker', image: 'skoda-llm-image' };
if (!process.env.OPENAI_BASE_URL || !process.env.OPENAI_KEY) throw new Error('Organization model configuration missing');
KEYS.find(x => x.path === 'llm.models').default = mapping;
const system = `Create concise accessible UI from data. Return a short plain-text summary and exactly one fenced code block marked skowork-ui containing strict JSON. No other code blocks. Use only this schema:
Root: {"version":1,"id":"unique-id","summary":"plain text","components":[component,...]}.
component types (no other properties):
{"type":"section" or "card","id":"id","label":"title","components":[component,...]}
{"type":"paragraph","id":"id","label":"accessible label","content":"display text"}
{"type":"table","id":"id","label":"title","columns":["column",...],"rows":[["value",...],...]}
{"type":"bar" or "line","id":"id","label":"title","unit":"unit","points":[{"label":"label","value":number},...]}
{"type":"diagram","id":"id","label":"title","nodes":[{"id":"id","label":"label"},...],"edges":[{"from":"node-id","to":"node-id"},...]}
{"type":"select" or "multiselect","id":"id","label":"label","options":["option",...]}
{"type":"switch","id":"id","label":"label","value":true or false}
{"type":"text","id":"id","label":"label","value":"initial text"}
{"type":"slider","id":"id","label":"label","min":number,"max":number,"value":number}
{"type":"button","id":"id","label":"submit label"}
Use unique ids. Show only supplied information, do not invent values. Include the component types requested. Use the language of the user.`;

const cases = [
  ['table', 'Compare projects Atlas and Boreal. Atlas costs 12 and Boreal costs 18. Show a table with project and cost.', ['Atlas','Boreal','12','18']],
  ['bar', 'Show a bar chart of the values: Alpha 12, Beta 18, unit hours.', ['Alpha','Beta','12','18','hours']],
  ['line', 'Show a line chart of the values: Monday 12, Tuesday 18, unit hours.', ['Monday','Tuesday','12','18','hours']],
  ['diagram', 'Draw a directed dependency diagram: Plan leads to Build, and Build leads to Review.', ['Plan','Build','Review']],
  ['select', 'Ask the user to select either weekly or monthly reporting; include a submit button.', ['weekly','monthly']],
  ['multiselect', 'Let the user select any of Planning, Build and Review; include a submit button.', ['Planning','Build','Review']],
  ['switch', 'Ask whether to include pending items using a switch, initially off; include a submit button.', ['pending','false']],
  ['text', 'Ask the user for a report title in a text field; include a submit button.', ['title']],
  ['slider', 'Ask for a priority from 1 to 5, initially 3, using a slider and a submit button.', ['priority','1','5','3']],
  ['card', 'Use a card with a paragraph to show the status of Project Atlas: ready.', ['Atlas','ready']],
];
const variants = ['', ' Keep it compact.', ' Make it suitable for a busy manager.', ' Use simple labels.'];
const prompts = variants.flatMap((v, vi) => cases.map(([type, prompt, facts], ci) => ({ type, facts, prompt: prompt + v, id: vi * 10 + ci + 1 })));
const own = (o, keys) => o && typeof o === 'object' && !Array.isArray(o) && Object.keys(o).every(k => keys.includes(k));
const str = x => typeof x === 'string' && x.length > 0 && x.length <= 4096;
const num = x => typeof x === 'number' && Number.isFinite(x);
function validate(raw, requested) {
  const match = raw.match(/```skowork-ui\s*\n([\s\S]*?)\n```/);
  if (!match) return 'missing or incomplete fence';
  if (Buffer.byteLength(match[1]) > 65536) return 'size';
  let root; try { root = JSON.parse(match[1]); } catch { return 'invalid JSON'; }
  if (!own(root,['version','id','summary','components']) || root.version !== 1 || !str(root.id) || !str(root.summary) || !Array.isArray(root.components)) return 'root schema';
  const ids = new Set(); const seen = new Set(); let count = 0; let controls = 0;
  function component(c, depth) {
    if (!c || typeof c !== 'object' || !str(c.type) || !str(c.id) || !str(c.label) || ids.has(c.id) || depth > 8 || ++count > 100) return false;
    ids.add(c.id); seen.add(c.type);
    if (['section','card'].includes(c.type)) return own(c,['type','id','label','components']) && Array.isArray(c.components) && c.components.every(x=>component(x,depth+1));
    if (c.type === 'paragraph') return own(c,['type','id','label','content']) && str(c.content);
    if (c.type === 'table') return own(c,['type','id','label','columns','rows']) && Array.isArray(c.columns) && c.columns.length>0 && c.columns.length<=20 && c.columns.every(str) && Array.isArray(c.rows) && c.rows.length<=500 && c.rows.every(r=>Array.isArray(r) && r.length===c.columns.length && r.every(str));
    if (['bar','line'].includes(c.type)) return own(c,['type','id','label','unit','points']) && str(c.unit) && Array.isArray(c.points) && c.points.length<=500 && c.points.every(p=>own(p,['label','value']) && str(p.label) && num(p.value));
    if (c.type === 'diagram') { if(!own(c,['type','id','label','nodes','edges']) || !Array.isArray(c.nodes) || c.nodes.length>100 || !Array.isArray(c.edges) || c.edges.length>200) return false; const ns=new Set(); return c.nodes.every(n=>own(n,['id','label']) && str(n.id) && str(n.label) && !ns.has(n.id) && !!ns.add(n.id)) && c.edges.every(e=>own(e,['from','to']) && ns.has(e.from) && ns.has(e.to)); }
    if (++controls>40) return false;
    if (['select','multiselect'].includes(c.type)) return own(c,['type','id','label','options']) && Array.isArray(c.options) && c.options.length>0 && c.options.length<=30 && c.options.every(str);
    if (c.type === 'switch') return own(c,['type','id','label','value']) && typeof c.value === 'boolean';
    if (c.type === 'text') return own(c,['type','id','label','value']) && typeof c.value === 'string';
    if (c.type === 'slider') return own(c,['type','id','label','min','max','value']) && [c.min,c.max,c.value].every(num) && c.min<=c.value && c.value<=c.max && c.min<c.max;
    if (c.type === 'button') return own(c,['type','id','label']);
    return false;
  }
  if (!root.components.every(x=>component(x,1))) return 'component schema';
  if (!seen.has(requested)) return 'requested component missing';
  if (requested === 'card' && !seen.has('paragraph')) return 'read-only text missing';
  if (['select','multiselect','switch','text','slider'].includes(requested) && !seen.has('button')) return 'submit missing';
  return 'valid';
}
const h = await startHarness(c => { c.gateway.upstreams=[{name:'organization',baseUrl:process.env.OPENAI_BASE_URL,apiKey:process.env.OPENAI_KEY,maxConcurrent:0}]; c.gateway.models={...mapping}; });
const results=[];
try {
  for (let i=0;i<100;i++) { const cs=h.cp.gateway.connections.list().connections; if(cs.length && cs.every(x=>x.status==='ok')) break; if(i===99) throw new Error('Gateway model discovery unavailable'); await new Promise(r=>setTimeout(r,100)); }
  const token=await h.token('genui-spike');
  for (let i=0;i<prompts.length;i++) {
    const item=prompts[i]; const role=Math.floor(i/10)%2===0?'main':'small'; const start=performance.now();
    try {
      const r=await h.call('POST','/llm/v1/chat/completions',token,{model:role,messages:[{role:'system',content:system},{role:'user',content:item.prompt}],max_tokens:2400,temperature:0});
      const text=String(r.body?.choices?.[0]?.message?.content??'');
      const verdict=r.status===200?validate(text,item.type):`HTTP ${r.status} ${r.body?.error?.code??''}`;
      const match=text.match(/```skowork-ui\s*\n([\s\S]*?)\n```/);
      let surface; try { surface=match?JSON.parse(match[1]):null; } catch { surface=null; }
      const displayed=JSON.stringify(surface??'').toLowerCase();
      const missing_facts=item.facts.filter(f=>!displayed.includes(f.toLowerCase()));
      results.push({id:item.id,role,type:item.type,prompt:item.prompt,status:r.status,verdict,missing_facts,elapsed_ms:Math.round(performance.now()-start),finish_reason:r.body?.choices?.[0]?.finish_reason??null,surface});
    } catch { results.push({id:item.id,role,type:item.type,prompt:item.prompt,verdict:'request failed',elapsed_ms:Math.round(performance.now()-start)}); }
    const {surface, ...progress}=results.at(-1); console.log(JSON.stringify(progress));
    await writeFile(new URL('./spike-results.json',import.meta.url),JSON.stringify({date:new Date().toISOString(),roles:mapping,results},null,2)+'\n');
    const wait=Math.max(0,3500-(performance.now()-start));
    if (wait) await new Promise(r=>setTimeout(r,wait));
  }
} finally {
  await writeFile(new URL('./spike-results.json',import.meta.url),JSON.stringify({date:new Date().toISOString(),roles:mapping,results},null,2)+'\n');
  await h.close();
}
