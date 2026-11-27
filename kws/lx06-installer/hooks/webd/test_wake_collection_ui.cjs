const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const html=fs.readFileSync(__dirname+'/index.html','utf8');
const part=(a,b)=>html.slice(html.indexOf(a),html.indexOf(b,html.indexOf(a)));
test('independent capture carries explicit purpose and condition and works without model',async()=>{
 let sent,callback;const ctx={wwData:{words:[{name:'测试测试',model:0}],state:{status:'idle'}},wwLog(){},wwStartPoll(){},api:async(path,body)=>{sent={path,body};return {ok:true};}};
 vm.createContext(ctx);vm.runInContext(part('  const wwRecord =','  const wwRecDel =')+'\nglobalThis.record=wwRecord;',ctx);
 ctx.record('测试测试',true,null,{purpose:'acceptance',condition:'far'});await new Promise(setImmediate);
 assert.equal(sent.path,'/api/wakeword/record');assert.equal(sent.body.purpose,'acceptance');assert.equal(sent.body.condition,'far');assert.equal(sent.body.test,1);assert.equal(sent.body.n,undefined);
 sent=null;ctx.record('测试测试',true,null,{purpose:'diagnostic',condition:'soft'});assert.equal(sent,null,'busy capture must not submit again');
});
test('collection count and private export link use percent encoded word',async()=>{
 const out={};let path;const row={getAttribute:()=> '测试测试',querySelector:s=>s==='[data-purpose]'?{value:'acceptance'}:out};
 const ctx={api:async p=>{path=p;return {ok:true,count:30,normal:20,far:5,soft:5};},wwEsc:s=>s};
 vm.createContext(ctx);vm.runInContext(part('  const wwCollectionLoad =','  const wwRecord =')+'\nglobalThis.load=wwCollectionLoad;',ctx);await ctx.load(row);
 assert.match(path,/name=%E6/);assert.match(out.innerHTML,/30\/100/);assert.match(out.innerHTML,/不等于声学验收通过/);assert.match(out.innerHTML,/samples\/export/);
});
test('render exposes explicit collection purposes separately from ten enrollment slots',()=>{
 const rows=[];const ctx={wwData:{words:[{name:'测试测试',reccount:10,model:0,samples:[]}]},wwStopPlay(){},wwList:{innerHTML:'',appendChild:r=>rows.push(r)},document:{createElement:()=>({setAttribute(){}})},wwEsc:s=>String(s),wwAsrNote:{}};
 vm.createContext(ctx);vm.runInContext(part('  const wwRender =','  const wwShowScore =')+'\nglobalThis.render=wwRender;',ctx);ctx.render();
 for(const purpose of ['acceptance','calibration','diagnostic'])assert.match(rows[0].innerHTML,new RegExp('value="'+purpose+'"'));
 assert.match(rows[0].innerHTML,/data-act="collect"/);assert.match(rows[0].innerHTML,/独立样本不评分、不训练、不修改门限/);
});
test('reserve during HTTP request, expose capture error and allow retry after rejection',async()=>{
 const logs=[];let resolve,calls=0;const initial={status:'idle'};
 const ctx={wwData:{state:initial},wwLog:s=>logs.push(s),wwStartPoll(){},api:()=>{calls++;return new Promise(r=>{resolve=r});}};
 vm.createContext(ctx);vm.runInContext(part('  const wwRecord =','  const wwRecDel =')+'\nglobalThis.record=wwRecord;',ctx);
 const collect={purpose:'acceptance',condition:'far'};
 ctx.record('测试测试',true,null,collect);ctx.record('测试测试',true,null,collect);
 assert.equal(calls,1);resolve({ok:false,error:'独立样本已达100条'});await new Promise(setImmediate);
 assert.equal(ctx.wwData.state,initial);assert.match(logs.at(-1),/独立样本已达100条/);
 ctx.record('测试测试',true,null,collect);assert.equal(calls,2);resolve({ok:true});await new Promise(setImmediate);
});
test('HTTP failure clears only its own reservation and shows error for old score tests',async()=>{
 const logs=[];const initial={status:'idle'};const ctx={wwData:{state:initial},wwLog:s=>logs.push(s),wwStartPoll(){},api:async()=>{throw Error('fixture connection failure');}};
 vm.createContext(ctx);vm.runInContext(part('  const wwRecord =','  const wwRecDel =')+'\nglobalThis.record=wwRecord;',ctx);
 ctx.record('测试测试',true);await new Promise(setImmediate);assert.equal(ctx.wwData.state,initial);assert.match(logs.at(-1),/连接失败/);
 ctx.api=async()=>({ok:false,error:'需先训练'});ctx.record('测试测试',true);await new Promise(setImmediate);assert.match(logs.at(-1),/测试启动失败：需先训练/);
});
