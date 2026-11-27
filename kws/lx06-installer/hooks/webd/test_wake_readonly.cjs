const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), vm = require('node:vm');
const html = fs.readFileSync(__dirname + '/index.html', 'utf8');
test('wake renderer escapes names and offers recording', () => {
  const rows = [];
  const list = {innerHTML:'', appendChild(r){ rows.push(r); }};
  const ctx = {wwData:{words:[{name:'旧词',th:0.2,link:'old<script>',enabled:1,reccount:3,model:1}],linked_apps:[],state:null}, wwList:list, wwStopPlay(){}, document:{createElement(){return {setAttribute(){},innerHTML:'',querySelector(){return {addEventListener(){},appendChild(){}}},querySelectorAll(){return []}}}}, wwOpen:new Set(), wwPlaybackPending:false};
  vm.createContext(ctx);
  const esc = html.slice(html.indexOf('  const wwEsc ='),html.indexOf('  const wwLog ='));
  const render = html.slice(html.indexOf('  const wwRender ='),html.indexOf('  const wwShowScore ='));
  vm.runInContext(esc + render + '\nwwRender();',ctx);
  assert.equal(rows.length, 1);
  assert.match(rows[0].innerHTML, /旧词/);
  assert.match(rows[0].innerHTML, /old&lt;script&gt;/);
  assert.match(rows[0].innerHTML, /data-act="rec"/);
});
test('local wake enrollment is available and the official switch stays separate', () => {
  assert.match(html, /本地唤醒词功能已开启/);
  assert.match(html, /id="wwAddBtn"/);
  assert.doesNotMatch(html, /id="wwAddBtn"[^>]*hidden/);
  assert.match(html, /id="wwOfficialChk"/);
});
