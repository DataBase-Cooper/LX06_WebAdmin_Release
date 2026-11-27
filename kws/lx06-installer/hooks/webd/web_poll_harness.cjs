const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
module.exports = function harness(options={}) {
  const html = fs.readFileSync(path.join(__dirname, 'index.html'), 'utf8');
  const nodes = new Map(), timers = new Map(), requests = [], events = new Map();
  let now = 0, serial = 0, logins = 0;
  const node = id => {
    if (!nodes.has(id)) {
      const classes = new Set(), b = { textContent: '' };
      nodes.set(id, { style: {}, hidden: false, textContent: '', attrs: {}, classes, b,
        setAttribute(k,v) { this.attrs[k]=v; }, removeAttribute() {}, focus() {},
        addEventListener() {}, classList: { add(k) { classes.add(k); }, remove(k) { classes.delete(k); },
          toggle(k,on) { if(on) classes.add(k); else classes.delete(k); } },
        querySelector: () => b, querySelectorAll: () => Array(8).fill({}) });
    }
    return nodes.get(id);
  };
  const schedule = (fn,ms,interval) => { const id=++serial; timers.set(id,{fn,ms,at:now+ms,interval}); return id; };
  const ctx = { $:node, coreUptime:node('coreUptime'), AbortController,
    Date: class extends Date { static now() { return now; } },
    document: { hidden:false, addEventListener(k,fn) { events.set(k,fn); } },
    setTimeout:(fn,ms)=>schedule(fn,ms,false), clearTimeout:id=>timers.delete(id),
    fetch(url,options) { return new Promise((resolve,reject)=> {
      requests.push({url,options,resolve,reject});
      options.signal?.addEventListener('abort',()=>{const e=new Error('abort');e.name='AbortError';reject(e);},{once:true});
    }); },
    showLoginPage() { logins++; ctx.poll.stop(); },
    wwSchedule() {}, refreshStorage() {},
    renderStorage(key,total,used) { node(key+'Storage').textContent=total+':'+used; },
  };
  ctx.window={setInterval:(fn,ms)=>schedule(fn,ms,true),clearInterval:id=>timers.delete(id),addEventListener(k,fn){events.set(k,fn);}};
  const block=(a,b)=>{const start=html.indexOf(a);const end=html.indexOf(b,start);if(start<0||end<0)throw Error(a);return html.slice(start,end);};
  vm.createContext(ctx);
  vm.runInContext('let coreTimer=null, storageRequest=null; const adminPage={hidden:false};\n'+
    block('  /* ---------- 统一轮询','  /* ---------- 空间')+
    block('  const cpuValue = $("cpuValue");','  /* ---------- fix15')+
    block('  /* ---------- 设备信息：一个请求','  /* ---------- V20261016.3 系统日志弹窗')+
    block('  const pausePage = () => {','\n  let lastFocusedElement')+
    `\npoller.enable(true); globalThis.poll={refreshCore,startCoreStats,stopCoreStats,startSystemStats,stopSystemStats,
      api,poller,cancelReads,stop(){cancelReads();poller.clear();stopSystemStats();}};`,ctx);
  return {requests,node,poll:ctx.poll,get logins(){return logins;},get intervals(){return [...timers.values()].filter(t=>t.interval).length;},
    async visibility(hidden){ctx.document.hidden=hidden;events.get('visibilitychange')();await new Promise(setImmediate);},
    async advance(ms) {
      const end=now+ms;
      while(true){let next=null;for(const item of timers)if(item[1].at<=end&&(!next||item[1].at<next[1].at))next=item;
        if(!next)break;const[id,t]=next;now=t.at;if(t.interval)t.at+=t.ms;else timers.delete(id);t.fn();await new Promise(setImmediate);}
      now=end;await new Promise(setImmediate);
    },
    reply(req,data){req.resolve({ok:true,status:200,json:async()=>data});}
  };
};
