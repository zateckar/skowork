// Verify task-specific synthetic content in the stored spike surfaces. This is a narrow automated usefulness proxy,
// not a human usability assessment, accessibility check, or guarantee that arbitrary generated content is correct.
import { readFile } from 'node:fs/promises';
const data=JSON.parse(await readFile(new URL('./spike-results.json',import.meta.url),'utf8')).results;
const flat=cs=>(cs||[]).flatMap(c=>[c,...flat(c.components)]);
const norm=x=>String(x).toLowerCase();
function assess(r) {
  if (r.verdict!=='valid') return r.verdict;
  const components=flat(r.surface?.components);
  const c=components.find(x=>x.type===r.type);
  if (!c) return 'missing requested component';
  if (r.type==='table') {
    const rows=c.rows.map(row=>row.map(norm).join(' '));
    if(!rows.some(x=>x.includes('atlas')&&x.includes('12')) || !rows.some(x=>x.includes('boreal')&&x.includes('18'))) return 'incorrect table data';
  }
  if (['bar','line'].includes(r.type)) {
    const points=Object.fromEntries(c.points.map(p=>[norm(p.label),p.value]));
    const expected=r.type==='bar'?{alpha:12,beta:18}:{monday:12,tuesday:18};
    if(Object.entries(expected).some(([k,v])=>points[k]!==v)||norm(c.unit)!=='hours') return 'incorrect chart data';
  }
  if (r.type==='diagram') {
    const nodes=Object.fromEntries(c.nodes.map(n=>[n.id,norm(n.label)]));
    const edges=c.edges.map(e=>nodes[e.from]+'>'+nodes[e.to]);
    if(!edges.includes('plan>build')||!edges.includes('build>review')) return 'incorrect dependency edges';
  }
  if (['select','multiselect'].includes(r.type)) {
    const expected=r.type==='select'?['weekly','monthly']:['planning','build','review'];
    if(expected.some(x=>!c.options.some(o=>norm(o).includes(x)))) return 'incorrect choices';
  }
  if (r.type==='switch' && c.value!==false) return 'incorrect initial switch value';
  if (r.type==='slider' && (c.min!==1||c.max!==5||c.value!==3)) return 'incorrect slider bounds or value';
  if (r.type==='text' && !norm(c.label).includes('title')) return 'missing title label';
  if (r.type==='card' && (!norm(c.label).includes('atlas')||!flat(c.components).some(x=>x.type==='paragraph'&&norm(x.content).includes('ready')))) return 'incorrect card content';
  return 'pass';
}
const result={};
for(const role of ['main','small']) {
  const rs=data.filter(r=>r.role===role), latencies=rs.map(r=>r.elapsed_ms).sort((a,b)=>a-b);
  const outcomes={}; for(const r of rs) {const x=assess(r);outcomes[x]=(outcomes[x]||0)+1;}
  result[role]={total:rs.length, outcomes, p50_ms:latencies[Math.ceil(latencies.length*.5)-1], p95_ms:latencies[Math.ceil(latencies.length*.95)-1]};
}
result.failures=data.map(r=>({id:r.id,role:r.role,type:r.type,reason:assess(r)})).filter(r=>r.reason!=='pass');
console.log(JSON.stringify(result,null,2));
if(data.length!==40 || data.some(r=>r.status!==200)) process.exitCode=1;
