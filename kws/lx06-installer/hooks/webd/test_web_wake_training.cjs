const test=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),vm=require('node:vm');
const html=fs.readFileSync(__dirname+'/index.html','utf8');
test('periodic list refresh preserves training polling and saved-recording progress',async()=>{
  const progress={},button={},attrs={};let callback,starts=0,cancels=0,status='training',count=3;
  const row={getAttribute:()=> '小智出来',setAttribute:(k,v)=>attrs[k]=v,querySelector:()=>progress,querySelectorAll:()=>[button]};
  const ctx={wwData:null,wwPollTimer:null,wwList:{querySelectorAll:()=>[row]},wwServiceState:{},wwServiceBtn:{style:{}},wwOfficialChk:{},
    adminPage:{hidden:false},document:{hidden:false},wwRender(){},wwLog(){},wwShowScore(){},$(){return null;},async p05JointLoad(){},
    poller:{every(fn){callback=fn;starts++;return 'poll';},cancel(){cancels++;},after(){return 'schedule';}},
    async api(path){return path.endsWith('/state')?{status,word:'小智出来'}:{ok:true,words:[{name:'小智出来',reccount:count}],linked_apps:[],state:{status,word:'小智出来'},service:true};}};
  vm.createContext(ctx);
  function section(a,b){return html.slice(html.indexOf(a),html.indexOf(b,html.indexOf(a)));}
  vm.runInContext(section('  const wwStopPoll =','  const wwRecord =')+
    section('  let wwLoading = false','  /* P0.5 wake-crosstalk BEGIN */')+'\nglobalThis.load=wwLoad;',ctx);
  await ctx.load();assert.equal(starts,1);assert.equal(progress.hidden,false);assert.match(progress.textContent,/已保存.*训练/);
  assert.equal(button.disabled,true);assert.equal(attrs['aria-busy'],'true');
  await ctx.load();assert.equal(starts,1,'20s refresh must not cancel training polling');
  assert.equal(ctx.wwData.words[0].reccount,3);
  status='done';await callback();await new Promise(setImmediate);
  assert.equal(progress.hidden,true);assert.equal(button.disabled,false);assert.equal(ctx.wwPollTimer,null);
  assert.equal(ctx.wwServiceState.textContent,'监听运行中');assert.ok(cancels>0);
});
