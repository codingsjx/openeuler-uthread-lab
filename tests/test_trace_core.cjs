const {test}=require('node:test');
const assert=require('node:assert/strict');
const {parseCSV,explain}=require('../tools/trace_core.js');
test('CSV quotes, embedded newline, BOM and recent-window marker',()=>{
 const data=parseCSV('\uFEFFtime_us,tid,event,detail\r\n0,-1,overwritten,99\r\n5,0,mark,"comma,""quote""\nnext"\r\n6,0,cancel,cancel_requested');
 assert.equal(data.overwritten,99);assert.equal(data.events[1].detail,'comma,"quote"\nnext');
 assert.match(explain(data.events[2]),/不等于已经退出/);
});
test('reject malformed, unsafe numeric and non-monotonic input',()=>{
 const head='time_us,tid,event,detail\n';
 for(const body of ['1,0,mark,"unclosed','1,0,mark,"ok"extra','2,0,done,x\n1,0,ready,y','1,32,ready,x','9007199254740992,0,ready,x'])assert.throws(()=>parseCSV(head+body));
});
test('reclaim explanation uses recorded relation, labels stay raw text',()=>{
 assert.match(explain({event:'mark',detail:'reclaim:a>b'}),/连接 10.*连接 11/);
 const data=parseCSV('time_us,tid,event,detail\n1,0,mark,<img src=x onerror=alert(1)>\n');
 assert.equal(data.events[0].detail,'<img src=x onerror=alert(1)>');
});
