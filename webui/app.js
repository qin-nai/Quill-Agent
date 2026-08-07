/* Quill Agent WebUI · 对接 hermes_server HTTP + SSE 接口
   会话/provider/文件树/发送/确认/停止全部走真实后端。 */
(function () {
  'use strict';

  /* ---------- 工具 ---------- */
  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  const ic = id => '<svg><use href="#' + id + '"/></svg>';
  const esc = s => { const d = document.createElement('div'); d.textContent = s; return d.innerHTML; };
  const streamEl = $('#stream');

  const TOOL_ICON = {
    read_file: 'i-file-text', write_file: 'i-file-plus', edit_file: 'i-pencil',
    search_code: 'i-search', run_script: 'i-terminal', web_fetch: 'i-globe',
    list_directory: 'i-folder',
    web_search: 'i-search', read_skill: 'i-file-text',
    glob: 'i-search', grep: 'i-search', todo: 'i-check', task: 'i-terminal',
  };
  const ST_LABEL = { running: '运行中', wait: '等待确认', ok: '完成', err: '失败' };
  const DEPTH_LABEL = { 1: 'Minimal', 2: 'Low', 3: 'Medium', 4: 'High', 5: 'Max' };

  /* ---------- API ---------- */
  const api = {
    async json(path, opts) {
      const r = await fetch(path, opts);
      let data = {};
      try { data = await r.json(); } catch (e) { /* 非 JSON */ }
      if (!r.ok) throw new Error((data && data.error) ? data.error : ('HTTP ' + r.status));
      return data;
    },
    // POST + SSE 流式:逐帧回调 onEvent(eventName, data)
    async ssePost(path, body, signal, onEvent) {
      const r = await fetch(path, {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body), signal,
      });
      if (!r.ok) {
        let data = {};
        try { data = await r.json(); } catch (e) {}
        throw new Error((data && data.error) ? data.error : ('HTTP ' + r.status));
      }
      const reader = r.body.getReader();
      const dec = new TextDecoder();
      let buf = '';
      for (;;) {
        let chunk;
        try {
          const { done, value } = await reader.read();
          if (done) break;
          chunk = value;
        } catch (e) {
          break;  // 流被网络层中断:已收到的事件足以驱动 UI,不报 Network error
        }
        buf += dec.decode(chunk, { stream: true });
        let idx;
        while ((idx = buf.indexOf('\n\n')) !== -1) {
          const frame = buf.slice(0, idx);
          buf = buf.slice(idx + 2);
          await handleFrame(frame, onEvent);
        }
      }
    },
  };

  function handleFrame(frame, onEvent) {
    let event = 'message', data = '';
    for (const line of frame.split('\n')) {
      if (line.startsWith(':')) continue;                 // 心跳/注释
      if (line.startsWith('event:')) event = line.slice(6).trim();
      else if (line.startsWith('data:')) data += line.slice(5).trim();
    }
    if (!data) return;
    let obj;
    try { obj = JSON.parse(data); } catch (e) { return; }
    return onEvent(event, obj);
  }

  /* ---------- 状态 ---------- */
  let providers = [];
  let sessions = [];            // 会话元信息列表(后端)
  let messages = [];            // 当前会话消息(后端 JSON)
  let activeId = null;
  let state = { provider: 'deepseek', model: 'deepseek-v4-flash', depth: 3 };
  let generating = false;
  let controller = null;
  let assistantTok = null;      // 流式助手气泡句柄
  let assistantBuf = '';
  let turnEnded = false;        // 本回合是否正常收尾(done/error),用于检测流被异常切断
  let toolHandles = {};         // tool_use id → appendTool 句柄
  let toolGroupEl = null;       // 当前助手回合的工具分组容器(同一回合的工具合并展示)

  /* ---------- 会话列表 ---------- */
  function renderSessions() {
    const box = $('#sessionList');
    box.innerHTML = '';
    if (!sessions.length) {
      box.innerHTML = '<p class="empty">还没有会话<br>点下方「新建会话」开始</p>';
      return;
    }
    sessions.forEach(s => {
      const it = document.createElement('button');
      it.className = 'session-item' + (s.id === activeId ? ' active' : '');
      it.setAttribute('role', 'option');
      it.setAttribute('aria-selected', s.id === activeId);
      it.innerHTML =
        '<span class="session-icn">' + ic('i-msg') + '</span>' +
        '<span class="session-main"><span class="session-title">' + esc(s.title) + '</span>' +
        '<span class="session-meta">' + esc(s.model) + ' · ' + esc(s.created_at) + '</span></span>';
      it.addEventListener('click', () => selectSession(s.id));
      box.appendChild(it);
    });
  }

  async function selectSession(id) {
    activeId = id;
    renderSessions();
    const s = sessions.find(x => x.id === id);
    $('#sessionTitle').textContent = s ? s.title : '会话';
    try {
      const data = await api.json('/api/sessions/' + encodeURIComponent(id) + '/messages');
      messages = data.messages || [];
    } catch (e) {
      messages = [];
    }
    renderMessages();
    closeDrawer('#sessions');
  }

  let newSessionBusy = false;  // 防抖:创建请求进行中忽略重复点击
  async function newSession() {
    if (newSessionBusy) return;
    newSessionBusy = true;
    try {
      const data = await api.json('/api/sessions', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ provider: state.provider, model: state.model }),
      });
      const s = data.session;
      sessions.unshift(s);
      activeId = s.id;
      messages = [];
      renderSessions();
      $('#sessionTitle').textContent = s.title;
      renderMessages();
    } catch (e) {
      $('#sessionTitle').textContent = '新建会话失败: ' + e.message;
    } finally {
      newSessionBusy = false;
    }
  }

  /* ---------- 消息渲染 ---------- */
  function targetOf(b) {
    const inp = b.input || b.params || {};
    if (typeof inp.path === 'string') return inp.path;
    for (const k of Object.keys(inp)) if (typeof inp[k] === 'string') return inp[k];
    return '';
  }

  function renderMessages() {
    streamEl.innerHTML = '';
    toolHandles = {};
    toolGroupEl = null;
    assistantTok = null;
    assistantBuf = '';
    if (!messages.length) { showEmpty(); return; }
    messages.forEach(m => {
      if (m.role === 'user') {
        const texts = m.content.filter(b => b.type === 'text').map(b => b.text).join('');
        m.content.forEach(b => { if (b.type === 'tool_result') applyToolResult(b); });
        if (texts) appendUser(texts);
      } else if (m.role === 'assistant') {
        m.content.forEach(b => {
          if (b.type === 'text') appendAssistantText(b.text);
          else if (b.type === 'tool_use') {
            toolHandles[b.id] = appendTool({ name: b.name, target: targetOf(b), status: 'running', params: b.input });
          }
        });
      }
    });
    scrollBottom();
  }

  function applyToolResult(b) {
    const h = toolHandles[b.id];
    if (!h) return;
    h.setStatus(b.is_error ? 'err' : 'ok');
    h.setOutput(b.output);
  }

  /* 最小 Markdown 渲染(零依赖)。先转义所有内容再包白名单标签,防注入。
     覆盖:标题/代码块/行内代码/粗体/斜体/删除线/链接/无序有序列表/引用/分隔线/表格/任务列表。 */
  function renderMarkdown(src) {
    const escMd = s => s.replace(/&/g, '&amp;').replace(/</g, '&lt;')
                        .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
    const inline = s => s
      .replace(/`([^`]+)`/g, '<code>$1</code>')
      .replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
      .replace(/(^|[^*])\*([^*]+)\*/g, '$1<em>$2</em>')
      .replace(/~~([^~]+)~~/g, '<del>$1</del>')
      .replace(/\[([^\]]+)\]\(([^)]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>');

    const lines = String(src).split('\n');
    const out = [];
    let para = [];
    let i = 0;
    const flushPara = () => {
      if (para.length) {
        out.push('<p>' + inline(escMd(para.join(' '))) + '</p>');
        para = [];
      }
    };
    // GFM 表格:分隔行 = 只有 |、-、:、空格;按 : 位置推断对齐
    const isSepRow = s => /^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$/.test(s);
    const splitRow = s => s.trim().replace(/^\|/, '').replace(/\|$/, '').split('|').map(c => c.trim());
    // 任务列表项:- [ ] / - [x],渲染为禁用的 checkbox
    const itemHtml = m => {
      const c = m.match(/^\[( |x|X)\]\s+(.*)$/);
      if (!c) return inline(escMd(m));
      const checked = /[xX]/.test(c[1]);
      return '<label class="task"><input type="checkbox" disabled' + (checked ? ' checked' : '') + '>' +
             inline(escMd(c[2])) + '</label>';
    };
    while (i < lines.length) {
      const t = lines[i].trim();
      if (t.startsWith('```')) {
        flushPara();
        const lang = t.slice(3).trim();
        const buf = [];
        i++;
        while (i < lines.length && !lines[i].trim().startsWith('```')) { buf.push(lines[i]); i++; }
        i++;  // 跳过收尾 ```
        out.push('<pre><code' + (lang ? ' class="lang-' + escMd(lang) + '"' : '') + '>' +
                 escMd(buf.join('\n')) + '</code></pre>');
        continue;
      }
      const h = t.match(/^#{1,6}\s/);
      if (h) {
        flushPara();
        const lvl = h[0].trim().length;
        out.push('<h' + lvl + '>' + inline(escMd(t.slice(lvl + 1))) + '</h' + lvl + '>');
        i++;
        continue;
      }
      if (/^([-*_])\1{2,}$/.test(t)) { flushPara(); out.push('<hr>'); i++; continue; }
      if (/^>\s?/.test(t)) {
        flushPara();
        const buf = [];
        while (i < lines.length && /^>\s?/.test(lines[i].trim())) { buf.push(lines[i].replace(/^>\s?/, '')); i++; }
        out.push('<blockquote>' + inline(escMd(buf.join(' '))) + '</blockquote>');
        continue;
      }
      if (/^[-*+]\s+/.test(t)) {
        flushPara();
        const items = [];
        while (i < lines.length && /^[-*+]\s+/.test(lines[i].trim())) {
          items.push('<li>' + itemHtml(lines[i].trim().replace(/^[-*+]\s+/, '')) + '</li>');
          i++;
        }
        out.push('<ul>' + items.join('') + '</ul>');
        continue;
      }
      if (/^\d+\.\s+/.test(t)) {
        flushPara();
        const items = [];
        while (i < lines.length && /^\d+\.\s+/.test(lines[i].trim())) {
          items.push('<li>' + itemHtml(lines[i].trim().replace(/^\d+\.\s+/, '')) + '</li>');
          i++;
        }
        out.push('<ol>' + items.join('') + '</ol>');
        continue;
      }
      // GFM 表格:当前行含 | 且下一行是分隔行
      if (t.includes('|') && i + 1 < lines.length && isSepRow(lines[i + 1])) {
        flushPara();
        const head = splitRow(t);
        const aligns = splitRow(lines[i + 1]).map(c => {
          const l = c.startsWith(':'), r = c.endsWith(':');
          return l && r ? 'center' : r ? 'right' : l ? 'left' : '';
        });
        const al = k => aligns[k] ? ' style="text-align:' + aligns[k] + '"' : '';
        i += 2;  // 跳过表头行 + 分隔行
        let html = '<div class="table-wrap"><table><thead><tr>';
        head.forEach((c, k) => html += '<th' + al(k) + '>' + inline(escMd(c)) + '</th>');
        html += '</tr></thead><tbody>';
        while (i < lines.length && lines[i].trim() !== '' && lines[i].includes('|')) {
          const cells = splitRow(lines[i]);
          html += '<tr>';
          cells.forEach((c, k) => html += '<td' + al(k) + '>' + inline(escMd(c)) + '</td>');
          html += '</tr>';
          i++;
        }
        html += '</tbody></table></div>';
        out.push(html);
        continue;
      }
      if (t === '') { flushPara(); i++; continue; }
      para.push(t);
      i++;
    }
    flushPara();
    return out.join('\n');
  }

  function showEmpty() {
    streamEl.innerHTML =
      '<div class="empty">' +
        '<div class="empty-icn">' + ic('i-zap') + '</div>' +
        '<h2>开始一个任务</h2>' +
        '<p>描述你想做的事,比如「修一下登录页的 bug」,Agent 会自己读代码、改文件。</p>' +
        '<button class="btn-primary" id="emptyNew">' + ic('i-plus') + '<span>新建会话</span></button>' +
      '</div>';
    $('#emptyNew').addEventListener('click', newSession);
  }

  function appendUser(text) {
    const row = document.createElement('div');
    row.className = 'msg user';
    row.innerHTML = '<div class="bubble">' + esc(text) + '</div>';
    streamEl.appendChild(row);
  }
  function appendAssistantText(text) {
    toolGroupEl = null;  // 文本出现 → 新一轮工具分组
    const row = document.createElement('div');
    row.className = 'msg assistant';
    row.innerHTML = '<div class="bubble"><div class="md">' + renderMarkdown(text) + '</div></div>';
    streamEl.appendChild(row);
  }
  function appendAssistant() {
    toolGroupEl = null;  // 文本出现 → 新一轮工具分组
    const row = document.createElement('div');
    row.className = 'msg assistant';
    const b = document.createElement('div');
    b.className = 'bubble';
    const t = document.createElement('span');
    const c = document.createElement('span');
    c.className = 'caret';
    b.append(t, c);
    row.appendChild(b);
    streamEl.appendChild(row);
    scrollBottom();
    return {
      set(v) { t.textContent = v; scrollBottom(); },
      finish() {
        c.remove();
        const raw = t.textContent;
        t.remove();
        if (raw) {
          const md = document.createElement('div');
          md.className = 'md';
          md.innerHTML = renderMarkdown(raw);
          b.appendChild(md);
        }
        scrollBottom();
      },
    };
  }

  function appendTool(t) {
    const st = t.status || 'running';
    const card = document.createElement('div');
    card.className = 'tool-card';
    card.innerHTML =
      '<button class="tool-head">' +
        '<span class="tool-icn">' + ic(TOOL_ICON[t.name] || 'i-zap') + '</span>' +
        '<span class="tool-main">' +
          '<span class="tool-name">' + esc(t.name) + '</span>' +
          (t.target ? '<span class="tool-sep">·</span><span class="tool-target">' + esc(t.target) + '</span>' : '') +
        '</span>' +
        '<span class="tool-status ' + st + '"><span class="st-dot"></span>' + ST_LABEL[st] + '</span>' +
        '<span class="iconbtn" aria-hidden="true">' + ic('i-chev-down') + '</span>' +
      '</button>' +
      '<div class="tool-body">' +
        '<div class="lbl">参数</div><pre>' + esc(JSON.stringify(t.params || {}, null, 2)) + '</pre>' +
        '<div class="lbl">结果</div><pre class="out">' + esc(t.output || '') + '</pre>' +
      '</div>';
    card.querySelector('.tool-head').addEventListener('click', () => card.classList.toggle('open'));
    ensureToolGroup().appendChild(card);
    scrollBottom();
    return {
      setStatus(s) {
        const el = card.querySelector('.tool-status');
        el.className = 'tool-status ' + s;
        el.innerHTML = '<span class="st-dot"></span>' + ST_LABEL[s];
      },
      setOutput(o) { card.querySelector('.out').textContent = o; },
    };
  }
  // 回合结束的 token 消耗统计(≥1k 显示 x.xk,否则显示具体数字),追加在消息流末尾
  function appendUsageNote(outTokens) {
    const txt = outTokens >= 1000 ? (outTokens / 1000).toFixed(1) + 'k' : String(outTokens);
    const row = document.createElement('div');
    row.className = 'usage-note';
    row.textContent = '本回合消耗 ' + txt + ' tokens';
    streamEl.appendChild(row);
    scrollBottom();
  }

  // 同一助手回合的连续工具调用合并到一个分组,避免多个卡片散乱堆叠
  function ensureToolGroup() {
    if (toolGroupEl && toolGroupEl.isConnected) return toolGroupEl;
    toolGroupEl = document.createElement('div');
    toolGroupEl.className = 'tool-group';
    streamEl.appendChild(toolGroupEl);
    return toolGroupEl;
  }

  function appendError(msg) {
    const row = document.createElement('div');
    row.className = 'msg assistant';
    row.innerHTML = '<div class="bubble err">' + esc('⚠ ' + msg) + '</div>';
    streamEl.appendChild(row);
    scrollBottom();
  }
  function scrollBottom() { streamEl.scrollTop = streamEl.scrollHeight; }

  function flushAssistant() {
    if (assistantTok) { assistantTok.finish(); assistantTok = null; }
    assistantBuf = '';
  }

  /* ---------- SSE 事件分派 ---------- */
  async function dispatchSse(name, data) {
    switch (name) {
      case 'text_delta':
        if (!assistantTok) assistantTok = appendAssistant();
        assistantBuf += data.text;
        assistantTok.set(assistantBuf);
        break;
      case 'tool_use':
        flushAssistant();
        toolHandles[data.id] = appendTool({ name: data.name, target: targetOf(data), status: 'running', params: data.input });
        break;
      case 'confirm_request': {
        flushAssistant();
        const h = toolHandles[data.id];
        if (h) h.setStatus('wait');
        const decision = await askConfirm({
          title: '执行 ' + data.name,
          sub: targetOf(data) || data.name,
          desc: 'Agent 请求执行以下操作,确认后继续:',
          diff: Object.keys(data.input || {}).map(k => {
            const v = typeof data.input[k] === 'string' ? data.input[k] : JSON.stringify(data.input[k]);
            return { t: '+', line: k + ': ' + v };
          }),
        });
        await api.json('/api/sessions/' + encodeURIComponent(activeId) + '/confirm', {
          method: 'POST', headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            allow: decision === 'allow',
            reason: decision === 'allow' ? '' : (decision.reason || ''),
          }),
        }).catch(() => {});
        if (h) h.setStatus('running');
        break;
      }
      case 'tool_result': {
        const h = toolHandles[data.id];
        if (h) { h.setStatus(data.ok ? 'ok' : 'err'); h.setOutput(data.output); }
        break;
      }
      case 'error':
        flushAssistant();
        turnEnded = true;
        appendError(data.message || 'LLM 错误');
        break;
      case 'usage': {
        const pct = data.context_window
          ? Math.max(0, Math.min(100, Math.round(data.context_used / data.context_window * 100)))
          : 0;
        $('#modelChipCtx').textContent = pct ? ' · ' + pct + '%' : '';
        if (data.out > 0) appendUsageNote(data.out);
        break;
      }
      case 'done':
        flushAssistant();
        turnEnded = true;
        setGenerating(false);
        break;
    }
  }

  /* ---------- 发送 / 停止 ---------- */
  function setGenerating(on) {
    generating = on;
    $('#send').hidden = on;
    $('#stop').hidden = !on;
    if (on) $('#send').disabled = true; else autosize();
  }

  async function submit() {
    const t = input.value.trim();
    if (!t || generating) return;
    if (!activeId) await newSession();
    if (!activeId) return;  // 新建会话失败
    input.value = '';
    autosize();
    appendUser(t);
    setGenerating(true);
    controller = new AbortController();
    const sid = activeId;
    turnEnded = false;
    try {
      await api.ssePost('/api/sessions/' + encodeURIComponent(sid) + '/messages',
        { text: t, depth: state.depth, model: state.model }, controller.signal, dispatchSse);
      // 流被切断且既没 done 也没 error(非主动停止)→ 后端连接中断,提示不完整
      if (!turnEnded && !controller.signal.aborted) {
        appendError('连接中断,回复可能不完整');
      }
    } catch (e) {
      if (e.name === 'AbortError') { flushAssistant(); return; }
      flushAssistant();
      appendError(e.message || '请求失败');
    } finally {
      setGenerating(false);
      refreshSessionsMeta();  // 首轮消息后服务端会用消息开头自动命名,刷新标题显示
    }
  }

  // 重新拉会话列表:服务端可能刚把"新会话"改成消息开头标题
  async function refreshSessionsMeta() {
    try {
      sessions = await api.json('/api/sessions');
      renderSessions();
      const cur = sessions.find(x => x.id === activeId);
      if (cur) $('#sessionTitle').textContent = cur.title;
    } catch (e) { /* 列表刷新失败不影响对话 */ }
  }

  function stopGeneration() {
    controller && controller.abort();
    controller = null;
    const sid = activeId;
    if (sid) fetch('/api/sessions/' + encodeURIComponent(sid) + '/stop', { method: 'POST' }).catch(() => {});
    flushAssistant();
    setGenerating(false);
  }

  /* ---------- 确认弹窗 ---------- */
  const confirmEl = $('#confirm');
  function askConfirm(info) {
    return new Promise(res => {
      $('#confirmTitle').textContent = info.title;
      $('#confirmSub').textContent = info.sub;
      $('#confirmDesc').innerHTML = info.desc;
      $('#confirmDiff').innerHTML = info.diff.map(d =>
        '<div class="d' + (d.t === '-' ? 'l' : 'a') + '">' + d.t + ' ' + esc(d.line) + '</div>').join('');
      $('#rejectReason').value = '';
      confirmEl.hidden = false;
      const done = val => { confirmEl.hidden = true; res(val); };
      $('#confirmAllow').onclick = () => done('allow');
      $('#confirmDeny').onclick = () => done({ deny: true, reason: $('#rejectReason').value.trim() });
      confirmEl.onclick = e => { if (e.target === confirmEl) done({ deny: true, reason: '' }); };
    });
  }

  /* ---------- 文件树 ---------- */
  async function openFiletree() {
    // 先打开抽屉再取数据:避免 fetch 挂起时侧边栏不显示
    openDrawer('#filetree');
    $('#tree').innerHTML = '<p class="empty">加载中…</p>';
    try {
      const data = await api.json('/api/files/tree');
      $('#tree').innerHTML = '';
      renderTree(data.root || [], $('#tree'));
    } catch (e) {
      $('#tree').innerHTML = '<p class="empty">' + esc(e.message) + '</p>';
    }
  }

  function renderTree(nodes, parent) {
    nodes.forEach(n => {
      const it = document.createElement('div');
      if (n.type === 'dir') {
        it.innerHTML =
          '<button class="tree-item" role="treeitem" aria-expanded="false">' +
            '<span class="chev">' + ic('i-chev-right') + '</span>' +
            ic('i-folder') + '<span class="name">' + esc(n.name) + '</span></button>' +
          '<div class="tree-children"></div>';
        it.querySelector('.tree-item').addEventListener('click', () => {
          it.querySelector('.tree-item').classList.toggle('open');
          const ex = it.querySelector('.tree-item').getAttribute('aria-expanded') === 'true';
          it.querySelector('.tree-item').setAttribute('aria-expanded', ex ? 'false' : 'true');
          renderTree(n.children || [], it.querySelector('.tree-children'));
        });
      } else {
        it.innerHTML =
          '<button class="tree-item" role="treeitem">' + ic('i-file') +
          '<span class="name">' + esc(n.name) + '</span></button>';
        it.querySelector('.tree-item').addEventListener('click', () => openFilePreview(n));
      }
      parent.appendChild(it);
    });
  }

  async function openFilePreview(f) {
    const sheet = document.createElement('div');
    sheet.className = 'confirm';
    sheet.innerHTML =
      '<div class="confirm-card">' +
        '<div class="confirm-head">' +
          '<span class="confirm-icon">' + ic('i-file-text') + '</span>' +
          '<div><h2>' + esc(f.name) + '</h2><p class="confirm-sub">只读预览</p></div>' +
        '</div>' +
        '<div class="confirm-body"><div class="diff" style="max-height:320px">' +
          '<pre class="dc" style="white-space:pre;font-family:var(--font-mono);padding:12px">加载中…</pre>' +
        '</div></div>' +
        '<div class="confirm-actions"><button class="btn-primary" style="flex:1">关闭</button></div>' +
      '</div>';
    document.body.appendChild(sheet);
    const pre = sheet.querySelector('pre');
    try {
      const data = await api.json('/api/files/content?path=' + encodeURIComponent(f.path));
      pre.textContent = data.content;
    } catch (e) {
      pre.textContent = '读取失败: ' + e.message;
    }
    sheet.querySelector('.btn-primary').onclick = () => sheet.remove();
    sheet.onclick = e => { if (e.target === sheet) sheet.remove(); };
  }

  /* ---------- 设置 ---------- */
  async function fetchProviders() {
    providers = await api.json('/api/providers');
    if (!providers.some(p => p.id === state.provider)) {
      state.provider = providers.length ? providers[0].id : 'deepseek';
      state.model = providers.length ? providers[0].default_model : '';
    }
    renderProviders();
  }

  function renderProviders() {
    const sel = $('#provider');
    sel.innerHTML = providers.map(p => '<option value="' + esc(p.id) + '">' + esc(p.name) + '</option>').join('');
    sel.value = state.provider;
    syncProvider();
  }
  function syncProvider() {
    const p = providers.find(x => x.id === state.provider) || providers[0];
    if (!p) return;
    const models = (p.models && p.models.length) ? p.models : [];
    if (!models.includes(state.model)) state.model = models[0] || '';
    // 每项:模型名(点击选中)+ 1M 复选框(勾选上下文 1M)+ 删除(内置=隐藏,自定义=真删)
    $('#modelList').innerHTML = models.map(m => {
      const is1M = (p.model_ctx || {})[m] >= 1000000;
      return '<div class="model-manage-item' + (m === state.model ? ' active' : '') + '">' +
        '<button class="model-manage-name" data-model="' + esc(m) + '" type="button" title="选择该模型">' +
          esc(m) + '</button>' +
        '<label class="model-ctx-toggle" title="上下文 1M,勾选生效">' +
          '<input type="checkbox" data-ctxmodel="' + esc(m) + '"' + (is1M ? ' checked' : '') + '><span>1M</span>' +
        '</label>' +
        '<button class="model-manage-del" data-del="' + esc(m) + '" type="button" title="删除" aria-label="删除">' +
          ic('i-x') + '</button>' +
        '</div>';
    }).join('');
    $$('#modelList .model-manage-name').forEach(b => b.addEventListener('click', () => {
      state.model = b.dataset.model;
      syncProvider();
      renderPopover();
    }));
    $$('#modelList .model-ctx-toggle input').forEach(c => c.addEventListener('change', () => {
      setModelCtx(c.dataset.ctxmodel, c.checked ? 1000000 : 0);
    }));
    $$('#modelList .model-manage-del').forEach(d => d.addEventListener('click', () => removeModel(d.dataset.del)));
    $('#modelChipLabel').textContent = state.model;
  }
  async function addModel() {
    const inp = $('#newModel');
    const model = inp.value.trim();
    if (!model) return;
    try {
      await api.json('/api/providers/' + encodeURIComponent(state.provider) + '/models', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model }),
      });
      inp.value = '';
      await fetchProviders();
      state.model = model;      // 添加后默认选中新模型
      syncProvider();
      renderPopover();
    } catch (e) { /* 静默 */ }
  }
  async function setModelCtx(model, window) {
    try {
      await api.json('/api/providers/' + encodeURIComponent(state.provider) + '/models/ctx', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model, context_window: window }),
      });
      await fetchProviders();
      syncProvider();
    } catch (e) { /* 静默 */ }
  }
  async function removeModel(model) {
    try {
      await api.json('/api/providers/' + encodeURIComponent(state.provider) + '/models/remove', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ model }),
      });
      await fetchProviders();
      if (state.model === model) state.model = '';  // 让 syncProvider 重新落到默认模型
      syncProvider();
      renderPopover();
    } catch (e) { /* 静默 */ }
  }
  // 模型 pill 弹层:模型类型列表 + 思考深度滑杆;宽度 = 模型 pill 宽度 + 25px
  function renderPopover() {
    $('#modelPopover').style.width = ($('#modelChip').offsetWidth + 25) + 'px';
    const p = providers.find(x => x.id === state.provider) || providers[0];
    const models = (p && p.models && p.models.length) ? p.models : ['(手动填写)'];
    $('#popoverModels').innerHTML = models.map(m =>
      '<button class="model-opt' + (m === state.model ? ' active' : '') + '" data-model="' + esc(m) + '">' + esc(m) + '</button>').join('');
    $$('#popoverModels .model-opt').forEach(b => b.addEventListener('click', () => {
      state.model = b.dataset.model;
      syncProvider();
      renderPopover();
    }));
    $('#depthSlider').value = state.depth;
    $('#depthValue').textContent = DEPTH_LABEL[state.depth];
  }

  async function saveKey() {
    const key = $('#apiKey').value.trim();
    try {
      await api.json('/api/providers/' + encodeURIComponent(state.provider) + '/key', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ key }),
      });
    } catch (e) { /* 静默 */ }
  }
  // 回填已保存的 key:打开设置 / 切换 provider 时调用,key 不再"第二次打开就消失"
  async function loadKey() {
    try {
      const data = await api.json('/api/providers/' + encodeURIComponent(state.provider) + '/key');
      $('#apiKey').value = data.key || '';
    } catch (e) { /* 静默 */ }
  }
  async function testConnection() {
    const btn = $('#testConn');
    const out = $('#testResult');
    const key = $('#apiKey').value.trim();
    const orig = btn.querySelector('span').textContent;
    btn.disabled = true;
    out.hidden = true;
    btn.querySelector('span').textContent = '连接中…';
    try {
      const data = await api.json('/api/providers/' + encodeURIComponent(state.provider) + '/test', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ key }),
      });
      out.className = 'test-result ok';
      out.textContent = '连接成功 · 延迟 ' + data.latency_ms + 'ms';
    } catch (e) {
      out.className = 'test-result err';
      out.textContent = e.message;
    } finally {
      btn.disabled = false;
      btn.querySelector('span').textContent = orig;
      out.hidden = false;
    }
  }

  /* ---------- 事件绑定 ---------- */
  $('#provider').addEventListener('change', () => {
    state.provider = $('#provider').value;
    const p = providers.find(x => x.id === state.provider);
    state.model = (p && p.models && p.models[0]) || '';
    syncProvider();
    loadKey();  // 切换 provider → 回填该 provider 已保存的 key
  });
  // 回车或失焦都自动添加,不需要"新建"按钮
  $('#newModel').addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); addModel(); }
  });
  $('#newModel').addEventListener('blur', () => addModel());

  let keyTimer = null;
  $('#apiKey').addEventListener('input', () => {
    clearTimeout(keyTimer);
    keyTimer = setTimeout(saveKey, 400);
  });
  $('#toggleKey').addEventListener('click', () => {
    const inp = $('#apiKey');
    const show = inp.type === 'password';
    inp.type = show ? 'text' : 'password';
    $('#toggleKey').innerHTML = ic(show ? 'i-eye-off' : 'i-eye');
    $('#toggleKey').setAttribute('aria-label', show ? '隐藏密钥' : '显示密钥');
  });
  $('#testConn').addEventListener('click', testConnection);

  $('#themeToggle').addEventListener('click', () => {
    const dark = document.documentElement.dataset.theme !== 'light';
    document.documentElement.dataset.theme = dark ? 'light' : 'dark';
    $('#themeToggle').setAttribute('aria-checked', dark ? 'false' : 'true');
  });

  // 设置项:点击标题展开/收起
  $$('.set-item-head').forEach(h => h.addEventListener('click', () => {
    const item = h.closest('.set-item');
    const open = item.classList.toggle('open');
    h.setAttribute('aria-expanded', open ? 'true' : 'false');
  }));

  // 对话字体:系统 / 等宽
  $$('#chatFont .seg-opt').forEach(b => b.addEventListener('click', () => {
    $$('#chatFont .seg-opt').forEach(x => x.classList.remove('active'));
    b.classList.add('active');
    document.documentElement.style.setProperty('--chat-fam',
      b.dataset.font === 'mono' ? 'var(--font-mono)' : 'var(--font-ui)');
  }));
  // 对话字号:小 / 中 / 大 / 自定义
  function applyChatFs(px) { document.documentElement.style.setProperty('--chat-fs', px + 'px'); }
  $$('#chatFsSeg .seg-opt').forEach(b => b.addEventListener('click', () => {
    $$('#chatFsSeg .seg-opt').forEach(x => x.classList.remove('active'));
    b.classList.add('active');
    if (b.dataset.fs === 'custom') {
      $('#chatFsCustom').hidden = false;
      $('#chatFsInput').value = parseInt(getComputedStyle(document.documentElement).getPropertyValue('--chat-fs')) || 16;
      $('#chatFsInput').focus();
    } else {
      $('#chatFsCustom').hidden = true;
      applyChatFs(+b.dataset.fs);
    }
  }));
  $('#chatFsInput').addEventListener('change', () => {
    const v = Math.min(24, Math.max(12, parseInt($('#chatFsInput').value) || 16));
    $('#chatFsInput').value = v;
    applyChatFs(v);
  });

  /* ---------- 抽屉 & 遮罩 ---------- */
  const scrim = $('#scrim');
  function openDrawer(sel) {
    const d = $(sel);
    if (sel === '#sessions' && window.innerWidth >= 640) return; // 平板下会话栏常驻
    d.classList.add('open');
    d.hidden = false;
    scrim.hidden = false;
  }
  function closeDrawer(sel) {
    $(sel).classList.remove('open');
    if (!$$('.drawer.open').length) scrim.hidden = true;
  }
  scrim.addEventListener('click', () => {
    if ($('#settings').classList.contains('open')) saveKey();  // 关闭设置前 flush 已输入的 key
    $('#sessions').classList.remove('open');          // 手机端会话侧栏是 .sessions,不是 .drawer
    $$('.drawer.open').forEach(d => d.classList.remove('open'));
    scrim.hidden = true;
  });
  $('#openSessions').addEventListener('click', () => openDrawer('#sessions'));
  $('#openFiletree').addEventListener('click', openFiletree);
  $('#closeFiletree').addEventListener('click', () => { $('#filetree').classList.remove('open'); scrim.hidden = true; });
  $('#openSettings').addEventListener('click', () => { openDrawer('#settings'); loadKey(); });
  $('#closeSettings').addEventListener('click', () => {
    saveKey();  // 关闭前 flush,避免 debounce 未触发导致 key 没存上
    $('#settings').classList.remove('open'); scrim.hidden = true;
  });
  // 点模型 pill:不打开设置,向上弹「模型类型 + 思考深度」面板
  $('#modelChip').addEventListener('click', e => {
    e.stopPropagation();
    const po = $('#modelPopover');
    if (po.hidden) renderPopover();
    po.hidden = !po.hidden;
  });
  document.addEventListener('click', e => {
    const po = $('#modelPopover');
    if (po.hidden) return;
    if (!po.contains(e.target) && !$('#modelChip').contains(e.target)) po.hidden = true;
  });
  $('#collapseSessions').addEventListener('click', () => $('#sessions').classList.toggle('collapsed'));

  /* ---------- 输入栏 ---------- */
  const input = $('#input');
  const sendBtn = $('#send');
  const stopBtn = $('#stop');
  function autosize() {
    input.style.height = 'auto';
    input.style.height = Math.min(input.scrollHeight, 140) + 'px';
    sendBtn.disabled = !input.value.trim() || generating;
  }
  input.addEventListener('input', autosize);
  input.addEventListener('keydown', e => {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); submit(); }
  });
  sendBtn.addEventListener('click', submit);
  stopBtn.addEventListener('click', stopGeneration);
  $('#attach').addEventListener('click', () => {
    const p = document.createElement('p');
    p.className = 'input-hint';
    p.textContent = '附件功能即将接入';
    $('#inputbar').after(p);
    setTimeout(() => p.remove(), 1600);
  });

  /* ---------- 初始化 ---------- */
  $('#newSession').addEventListener('click', newSession);
  $('#depthSlider').addEventListener('input', () => {
    state.depth = parseInt($('#depthSlider').value, 10);
    $('#depthValue').textContent = DEPTH_LABEL[state.depth];
  });

  (async function init() {
    try {
      await fetchProviders();
    } catch (e) {
      renderProviders();
    }
    try {
      sessions = await api.json('/api/sessions');
      renderSessions();
      if (sessions.length) {
        await selectSession(sessions[0].id);
      } else {
        showEmpty();
      }
    } catch (e) {
      renderSessions();
      showEmpty();
    }
    autosize();
  })();
})();
