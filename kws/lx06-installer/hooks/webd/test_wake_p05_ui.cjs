const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const html=fs.readFileSync(__dirname+'/index.html','utf8');
function part(a,b){return html.slice(html.indexOf(a),html.indexOf(b,html.indexOf(a)));}
test('all inline scripts parse',()=>{
  for(const s of html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g))new vm.Script(s[1]);
});
test('original and calibrated c matrices remain separate and escape labels',async()=>{
  const nodes=new Map();function $(id){if(!nodes.has(id))nodes.set(id,{addEventListener(){}});return nodes.get(id);}
  const raw={phase:'original',words:[{name:'<甲>',threshold:.24,next_threshold:.24,conflict:true}],cells:[{actual:'<乙>',model:'<甲>',c:.8}]};
  const after={phase:'calibrated',words:[{name:'<甲>',threshold:.24,next_threshold:.1,conflict:true,calibrated:true}],cells:[{actual:'<乙>',model:'<甲>',c:1.92}]};
  const ctx={$ ,api:async()=>({ok:true,status:'complete',original:raw,calibrated:after})};vm.createContext(ctx);
  vm.runInContext(part('  const p05Esc =','  $("p05Joint").addEventListener')+'\nglobalThis.load=p05JointLoad;',ctx);
  await ctx.load();assert.match($('p05Original').innerHTML,/0\.800/);assert.doesNotMatch($('p05Original').innerHTML,/1\.920/);
  assert.match($('p05Calibrated').innerHTML,/1\.920/);assert.match($('p05Calibrated').innerHTML,/默认禁用/);
  assert.doesNotMatch($('p05Original').innerHTML,/<甲>|<乙>/);assert.match($('p05Original').innerHTML,/&lt;甲&gt;/);
  assert.match($('p05JointStatus').textContent,/不计独立声学验收/);
});
test('force-enable needs explicit confirmation and sends force_conflict',async()=>{
  let listener,sent;const ctx={wwList:{addEventListener(k,fn){listener=fn;}},wwData:{words:[{name:'测试测试',enabled:0,conflict:1}]},confirm:()=>false,
    api:async(path,body)=>{sent=body;return {ok:true};},wwLoad(){},wwLog(){}};vm.createContext(ctx);
  vm.runInContext(part('  wwList.addEventListener("click"','  wwAddBtn.addEventListener'),ctx);
  const row={getAttribute:()=> '测试测试'},btn={getAttribute:()=> 'enable',closest:()=>row};const event={target:{closest:()=>btn}};
  listener(event);assert.equal(sent,undefined);ctx.confirm=()=>true;listener(event);await new Promise(setImmediate);
  assert.equal(sent.force_conflict,'1');assert.equal(sent.enabled,'1');
});

test('forensics capacity and drops remain visible with no events',async()=>{
  const nodes=new Map();function $(id){if(!nodes.has(id))nodes.set(id,{});return nodes.get(id);}
  const reply={ok:true,enabled:1,keep:5,cloud:0,count:0,pinned:0,bytes:11,max_bytes:4194304,dropped:42,events:[]};
  const ctx={$ ,p05JointLoad(){},api:async()=>reply,p05List:$('p05List')};vm.createContext(ctx);
  vm.runInContext(part('  const p05Load =','  $("p05Save").addEventListener')+'\nglobalThis.load=p05Load;',ctx);
  await ctx.load();assert.match($('p05Capacity').textContent,/0\/5 条/);
  assert.match($('p05Capacity').textContent,/11\/4194304 字节/);
  assert.match($('p05Capacity').textContent,/已丢弃 42 条/);
  assert.match($('p05List').innerHTML,/还没有取证记录/);
  ctx.api=async()=>null;await ctx.load();assert.equal($('p05Capacity').textContent,'');
});

test('forensics status does not render malformed capacity fields',async()=>{
  const nodes=new Map();function $(id){if(!nodes.has(id))nodes.set(id,{});return nodes.get(id);}
  const ctx={$ ,p05JointLoad(){},api:async()=>({ok:true,enabled:1,keep:5,events:[],count:0,pinned:0,
    bytes:11,max_bytes:4194304,dropped:'<script>fixture</script>'}),p05List:$('p05List')};vm.createContext(ctx);
  vm.runInContext(part('  const p05Load =','  $("p05Save").addEventListener')+'\nglobalThis.load=p05Load;',ctx);
  await ctx.load();assert.equal($('p05Capacity').textContent,'');
});
