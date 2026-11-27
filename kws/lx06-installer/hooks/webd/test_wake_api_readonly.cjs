const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const c=fs.readFileSync(__dirname+'/webd.c','utf8');
test('list handler declares read-only management without repair on GET',()=>{
 const s=c.slice(c.indexOf('static void h_ww_list(int fd)'),c.indexOf('/* POST /api/wakeword/official'));
 assert.match(s,/management_ui_read_only/);
 assert.doesNotMatch(s,/ww_repair_missing_links\s*\(/);
});
