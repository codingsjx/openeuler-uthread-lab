(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.TraceCore = api;
})(globalThis, function () {
  'use strict';
  function parseCSV(text) {
    const rows = []; let row = [], field = '', quoted = false, closed = false;
    text = text.replace(/^\uFEFF/, '');
    for (let i = 0; i < text.length; i++) {
      const c = text[i];
      if (quoted) {
        if (c === '"' && text[i + 1] === '"') { field += '"'; i++; }
        else if (c === '"') { quoted = false; closed = true; }
        else field += c;
      } else if (c === '"') {
        if (field || closed) throw Error('CSV 引号位置不正确');
        quoted = true;
      } else if (c === ',' || c === '\n' || c === '\r') {
        row.push(field); field = ''; closed = false;
        if (c !== ',') {
          if (c === '\r' && text[i + 1] === '\n') i++;
          if (row.some(value => value !== '')) rows.push(row);
          row = [];
        }
      } else {
        if (closed) throw Error('CSV 引号后存在多余字符');
        field += c;
      }
    }
    if (quoted) throw Error('CSV 引号没有闭合');
    if (field || row.length || closed) { row.push(field); rows.push(row); }
    const header = rows.shift();
    if (!header || header.length !== 4 || header.join(',') !== 'time_us,tid,event,detail') throw Error('需要运行时 CSV：time_us,tid,event,detail');
    let previous = -1, overwritten = 0, dropped = 0;
    const events = rows.map((parts, index) => {
      if (parts.length !== 4) throw Error('第 ' + (index + 2) + ' 行列数不正确');
      if (!/^\d+$/.test(parts[0]) || !/^-?\d+$/.test(parts[1])) throw Error('时间或任务编号无效');
      const time = Number(parts[0]), tid = Number(parts[1]);
      if (!Number.isSafeInteger(time) || !Number.isSafeInteger(tid) || tid < -1 || tid > 31 || time < previous) throw Error('时间不单调或任务编号超出范围');
      previous = time;
      const event = parts[2], detail = parts[3];
      if (!['ready', 'running', 'wait_io', 'done', 'mark', 'cancel', 'overwritten', 'dropped'].includes(event)) throw Error('未知事件：' + event);
      if (event === 'overwritten' || event === 'dropped') {
        const count = Number(detail);
        if (!/^\d+$/.test(detail) || !Number.isSafeInteger(count)) throw Error('覆盖或丢失计数无效');
        if (event === 'overwritten') overwritten += count; else dropped += count;
        if (tid !== -1 || !Number.isSafeInteger(overwritten + dropped)) throw Error('覆盖或丢失标记无效');
      }
      return { time, tid, event, detail, index };
    });
    if (!events.length) throw Error('文件没有事件');
    return { events, overwritten, dropped };
  }
  function explain(event) {
    const d = event.detail;
    const connectionId = value => Number.isSafeInteger(parseInt(value, 16)) ? String(parseInt(value, 16)) : '0x'+value;
    const generation = /^generation:([0-9a-f]+):([0-9a-f]+)$/.exec(d);
    if (generation) return '连接 ' + connectionId(generation[1]) + ' 在任务槽 T' + event.tid + ' 的第 ' + connectionId(generation[2]) + ' 代运行。槽号可以复用，代次不同表示不同任务。';
    const age = /^(stage_age|byte_age):([0-9a-f]+):([0-9a-f]+)$/.exec(d);
    if (age) return '决策时记录：连接 ' + connectionId(age[2]) + (age[1] === 'stage_age' ? ' 当前阶段持续 ' : ' 距上次字节进展 ') + connectionId(age[3]) + ' ms。这是记录的决策依据，不是事后推算的客户端时延。';
    const budget = /^budget_ms:(header|body|write|idle|lifetime|stall):([0-9a-f]+)$/.exec(d);
    if (budget) return '本次运行的 ' + budget[1] + ' 预算 / 阈值为 ' + connectionId(budget[2]) + ' ms；该设置记录可能在最近窗口中被覆盖。';
    if (/^policy:(reject|byte-stalled|phase-aware)$/.test(d)) return '本次请求服务使用的准入策略：' + d.slice(7) + '。比较时还需保持容量、探测和阶段期限配置一致。';
    const stage = /^(phase|phase_timeout|victim_phase):([0-9a-f]+):(header|body|write|idle)$/.exec(d);
    if (stage) {
      const names = {header:'请求头',body:'请求体',write:'响应写出',idle:'会话空闲'};
      const prefix = '连接 ' + connectionId(stage[2]) + ' 的' + names[stage[3]] + '阶段';
      if (stage[1] === 'phase_timeout') return prefix + '达到固定期限。少量字节和部分写入不会重置该期限。';
      if (stage[1] === 'victim_phase') return prefix + '被选为回收候选；还需结合取消、关闭和新任务准入记录判断交接是否完成。';
      return prefix + '开始。阶段标记描述连接状态，不等于 Linux 内核线程状态。';
    }
    const relation = /^reclaim:([0-9a-f]+)>([0-9a-f]+)$/.exec(d);
    if (relation) return '策略决定：回收连接 ' + connectionId(relation[1]) + '，为已有请求数据的新连接 ' + connectionId(relation[2]) + ' 释放容量。阈值是策略选择，不能证明旧连接存在恶意。';
    if (d.startsWith('request_ready:')) return '已收齐长度合法的完整请求。协议服务中的探测任务到达这里后，才有资格申请处理容量。';
    if (d.startsWith('request_complete:')) return '服务端已将完整响应写入内核，接下来进入会话空闲；这不等于客户端已经读取响应。';
    if (d.startsWith('probe_started:')) return '已创建独立的有界探测任务。它计入任务、连接和缓冲预算，接收任务可继续接收其他连接。';
    if (d.startsWith('probe_promoted:')) return '完整请求获得处理容量，原探测任务转为处理任务，没有额外创建任务。';
    if (d.startsWith('probe_rejected:')) return '候选已完成请求，但没有获得处理容量或合适回收对象，因此关闭候选连接。';
    if (d.startsWith('operation_yield:')) return '部分传输达到协作检查点，主动让出执行机会；不代表强制 CPU 时间片。';
    if (event.event === 'overwritten') return '记录只保留最近事件，已有 ' + d + ' 条旧事件被覆盖。不能据此推断完整任务历史或完整耗时。';
    if (event.event === 'dropped') return '记录发生丢失，本文件不能当作完整轨迹。';
    if (event.event === 'cancel') return '已请求协作取消。等待中的运行时 I/O 将返回 ECANCELED；任务仍需执行清理并正常返回，取消不等于已经退出。';
    if (d === 'read_eagain') return '任务等待可读：非阻塞读取暂时没有数据，执行机会交还调度器。';
    if (d === 'write_eagain') return '任务等待可写：本次写入暂时无法继续，等待发送缓冲可用；其他可运行任务仍可执行。';
    if (d === 'accept_eagain') return '接收任务等待新连接。';
    if (d.startsWith('capacity_reject:')) return '活动连接已满且没有执行回收，本次新连接被关闭。拒绝不会自动区分快慢客户端。';
    if (d.startsWith('probation:')) return '满载后的候选连接进入有界探测，最多等待 50 ms 的首字节；没有请求数据不会触发回收。';
    if (d.startsWith('budget_yield:')) return '连接达到本轮写出额度，主动让出执行机会。它不是内核抢占。';
    if (d.includes('_timeout:')) return '连接达到标记所示的等待或寿命期限，将执行资源回收。';
    if (d.startsWith('connection_admitted:')) return '准入成功，已分配连接任务。';
    if (d.startsWith('connection_closed:')) return '连接描述符已关闭，应用槽即将随任务返回释放。';
    return '原始记录：' + event.event + ' / ' + d + '。只解释已记录事件，不推断未观察到的原因。';
  }
  return { parseCSV, explain };
});
