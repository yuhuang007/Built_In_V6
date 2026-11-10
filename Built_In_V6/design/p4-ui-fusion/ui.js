(() => {
  'use strict';
  const info = {name:'和光',latest:'↓ 回到最新'};
  const query = new URLSearchParams(location.search);
  const embedded = query.get('embed') === '1';
  const validRoutes = ['home','transcript','camera','settings','wifi','menu'];
  const initialRoute = query.get('page') || location.hash.slice(1) || 'menu';
  const sampleTranscript = [
    '今天的安排从这里开始。我们先确认集合地点，再讨论出发时间。',
    '请慢一点说，我正在看屏幕上的文字。如果有不清楚的地方，我会用键盘补充。',
    '沿着步道向前走，经过花园后在路口左转。那边有一块清楚的指示牌。',
    '午后可能会下小雨，出发时记得带伞。我们可以选择有遮雨棚的路线。',
    '这是一段用于阅读测试的长句，文字换行后仍应保持完整，也可以向上翻阅前面的内容，新的转写不会打断当前阅读。',
    '到了入口以后先稍作休息，再一起决定接下来的安排。'
  ];
  const state = {
    route: validRoutes.includes(initialRoute) ? initialRoute : 'home',
    drafts:{home:'',transcript:''}, language:'zh', pinyin:'', candidatePage:0, keyboard:false, usb:false,
    offline:false, failNext:false, messages:[
      {who:'assistant',text:'你好，我在这里。你可以说话，也可以打字。'},
      {who:'user',text:'请帮我确认一下今天的安排。'},
      {who:'assistant',text:'好的，我们一项一项来。你想先从哪里开始？'}
    ],
    transcript:sampleTranscript.slice(), follow:true, scrollTop:0, homeScroll:0, recording:false, holding:false,
    replying:false, revision:0, timer:null, replyTimer:null, networkTimer:null, modal:null,
    feedback:'', error:false, pendingNetwork:'', connected:'演示网络 · 花园', networkStatus:'connected',
    font:28, brightness:70, volume:60, idle:'120', cameraPaused:false, cameraGrid:true, changingReader:false
  };
  const icons = {
    home:'<path d="M5 5h22v15H16l-7 6v-6H5z"/><path d="M10 11h12m-12 4h8"/>',
    transcript:'<path d="M8 4h16v24H8zM12 10h8m-8 6h8m-8 6h5"/>',
    camera:'<path d="M4 10h7l2-4h6l2 4h7v17H4z"/><circle cx="16" cy="18" r="5"/>',
    settings:'<path d="M5 9h22M5 23h22"/><circle cx="12" cy="9" r="4"/><circle cx="21" cy="23" r="4"/>',
    mic:'<rect x="12" y="3" width="8" height="17" rx="4"/><path d="M8 15v2a8 8 0 0 0 16 0v-2M16 25v4m-5 0h10"/>',
    keyboard:'<rect x="3" y="6" width="26" height="21" rx="3"/><path d="M8 12h1m5 0h1m5 0h1m3 0h1M8 17h1m5 0h1m5 0h1M9 22h14"/>',
    back:'<path d="m19 7-9 9 9 9"/>',
    wifi:'<path d="M3 10a21 21 0 0 1 26 0M7 15a14 14 0 0 1 18 0m-14 5a8 8 0 0 1 10 0"/><circle cx="16" cy="26" r="1"/>',
    menu:'<rect x="5" y="5" width="8" height="8" rx="2"/><rect x="19" y="5" width="8" height="8" rx="2"/><rect x="5" y="19" width="8" height="8" rx="2"/><rect x="19" y="19" width="8" height="8" rx="2"/>',
    stop:'<rect x="8" y="8" width="16" height="16" rx="2"/>',
    send:'<path d="m4 5 24 11-24 11 4-11zM8 16h20"/>',
    sun:'<circle cx="16" cy="16" r="6"/><path d="M16 2v3m0 22v3M2 16h3m22 0h3M6 6l2 2m16 16 2 2M6 26l2-2M24 8l2-2"/>'
  };
  const icon = name => `<svg viewBox="0 0 32 32" aria-hidden="true">${icons[name] || icons.home}</svg>`;
  const esc = value => String(value).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
  const $ = selector => document.querySelector(selector);
  const title = {home:'语音对话',transcript:'独立转写',camera:'相机预览',settings:'设置',wifi:'Wi-Fi',menu:'灵译通'};
  document.body.classList.toggle('embed',embedded);
  document.body.innerHTML = `
    <div class="studio">
      <aside class="notes">
        <a class="studio-brand" href="../p4-ui-references/index.html">灵译通 / DESIGN STUDIO</a>
        <div class="project-title"><span>FUSION 01</span><h1>和光</h1><p>让每一种表达，<br>都有清晰的回应。</p></div>
        <p class="description">融合柔和浅色、黑白操作层级与专用录音设备的清晰状态。首页、对话、转写、相机及设置已组成一套可操作的界面。</p>
        <fieldset><legend>原型体验</legend>
          <label>当前页面 <select id="route-picker" aria-label="切换演示页面"><option value="menu">首页</option><option value="home">语音对话</option><option value="transcript">独立转写</option><option value="camera">相机预览</option><option value="settings">设置</option><option value="wifi">Wi-Fi</option></select></label>
          <div class="checks"><label><input id="usb-demo" type="checkbox">USB 键盘接入</label><label><input id="offline-demo" type="checkbox">模拟离线</label><label><input id="failure-demo" type="checkbox">下一次发送失败</label></div>
          <button id="revision-demo" class="full">模拟转写 / 修订一句</button><button id="long-demo" class="full">载入长文，体验阅读</button>
        </fieldset>
        <div class="studio-note"><b>600 × 1024 · 本地交互原型</b><p>所有语音、相机、网络均为模拟。无真实设备连接，无内容保存。</p><p>正文默认 28px / 行高 54px，输入框 64px、候选行 62px。浏览器效果尚未通过 P4 实机验收。</p></div>
        <p class="studio-link"><a href="README.md">设计说明</a><span>·</span><a href="../p4-ui-references/index.html">参考来源</a></p>
      </aside>
      <div class="device-wrap"><section class="device" id="device" aria-label="600乘1024 灵译通融合界面"></section></div>
    </div>`;
  const device = $('#device');
  function navigation() {
    return `<nav class="nav" aria-label="主要功能">${[['menu','首页','menu'],['home','对话','home'],['transcript','转写','transcript'],['settings','设置','settings']].map(([r,label,i])=>`<button data-route="${r}" ${state.route===r || (r==='settings'&&state.route==='wifi') || (r==='menu'&&state.route==='camera')?'aria-current="page"':''}>${icon(i)}<span>${label}</span></button>`).join('')}</nav>`;
  }
  function render() {
    const r = state.route;
    device.dataset.page=r;
    const subtitle = {menu:'每一种表达，都被听见',home:'语音与文字，随时切换',transcript:'安心聆听，从容阅读',camera:'让画面保持清晰',settings:'调成你习惯的样子',wifi:'连接身边的网络'}[r];
    device.style.setProperty('--body-size',`${state.font}px`);
    device.classList.toggle('keyboard-open',state.keyboard);
    device.classList.toggle('candidate-open',!!state.pinyin && state.language==='zh');
    device.innerHTML = `
      <div class="status"><span class="wordmark">LINGYITONG <span>/ 灵译通</span></span><span class="signal"><i class="connection-dot ${state.offline?'offline':''}"></i><span id="connection-indicator">${state.offline?'离线':'演示连接'}</span></span></div>
      <header class="header">${r==='menu'?`<span class="brand-mark">译</span>`:`<button class="icon quiet back" data-route="${r==='wifi'?'settings':'menu'}" aria-label="${r==='wifi'?'返回设置':'返回首页'}">${icon('back')}</button>`}<div class="heading"><h1>${title[r]}</h1><div class="subtitle">${subtitle}</div></div>${r==='menu'||r==='home'||r==='transcript'?`<button class="icon quiet" data-route="settings" aria-label="打开设置">${icon('settings')}</button>`:''}</header>
      <div class="shell"><main class="workspace" id="workspace">${pageMarkup()}</main>${navigation()}</div>
      <div id="modal-root"></div>`;
    bindCommon();
    bindPage();
    $('#route-picker').value = r;
    if (r==='home' || r==='transcript') {
      const reader=$('.reader');
      lockReader(() => {reader.scrollTop=r==='transcript' ? (state.follow ? reader.scrollHeight : state.scrollTop) : state.homeScroll;});
      if (r==='home' && state.homeScroll===0) reader.scrollTop=reader.scrollHeight;
      updateReadingBar();
    }
    if(state.modal) renderModal();
    scale();
  }
  function pageMarkup() {
    switch(state.route) {
      case 'home': return `<section class="page"><div class="reader" id="chat-reader" tabindex="0" aria-label="对话记录">
        <div class="conversation-start"><span></span>今天的对话<span></span></div>
        <div id="messages">${messagesMarkup()}</div></div><div class="feedback ${state.error?'error':''}" role="status">${esc(state.feedback)}</div>
        <div class="toolbar">${state.replying?`<button class="record primary" id="interrupt">${icon('stop')}打断回复</button>`:`<button class="record primary" id="hold-talk" aria-label="按住说话，松开发送">${icon('mic')}<span>按住说话</span></button>`}</div>${composerMarkup()}</section>`;
      case 'transcript': return `<section class="page"><div class="page-heading"><span>当前会话</span><span id="record-status">${state.recording?'● 正在聆听':'等待开始'}</span></div>
        <div class="reader transcript" id="transcript-reader" tabindex="0" aria-label="转写正文">${transcriptMarkup()}</div>
        <div class="readingbar"><span id="reading-state">自动跟随</span><button id="latest" hidden>${info.latest}</button></div>
        <div class="feedback ${state.error?'error':''}" role="status">${esc(state.feedback)}</div>
        <div class="toolbar"><button class="record ${state.recording?'':'primary'}" id="record" aria-pressed="${state.recording}">${icon(state.recording?'stop':'mic')}<span>${state.recording?'停止转写':'开始转写'}</span></button><button class="quiet" id="clear-transcript" ${state.recording?'disabled':''}>清空</button></div>${composerMarkup()}</section>`;
      case 'camera': return `<section class="page"><div class="page-heading"><span>预览画面</span><span>模拟</span></div>
        <div class="camera-scene ${state.cameraPaused?'paused':''}"><div class="camera-overlay"><span>${state.cameraPaused?'已暂停':'预览中'}</span><span>600 × 1024</span></div><div class="camera-subject"></div><div class="camera-caption">${state.cameraPaused?'预览已暂停':'请将人物置于画面中央'}</div></div>
        <div class="scroll-page" style="flex:0 0 auto"><div class="notice">相机示意画面 · 当前仅演示预览与取景，不包含手语识别。</div><div class="toolbar" style="margin:0"><button class="primary" id="pause-camera" style="flex:1">${state.cameraPaused?'继续预览':'暂停预览'}</button><button id="camera-guide">${state.cameraGrid?'隐藏辅助线':'显示辅助线'}</button></div></div></section>`;
      case 'settings': return `<section class="page"><div class="scroll-page">
        <div class="settings-group"><h2>屏幕与声音</h2>
          <label class="setting vertical"> <span>亮度 <output id="brightness-output">${state.brightness}%</output></span><input id="brightness" type="range" min="20" max="100" value="${state.brightness}" aria-label="亮度"></label>
          <label class="setting vertical"><span>音量 <output id="volume-output">${state.volume}%</output></span><input id="volume" type="range" min="0" max="100" value="${state.volume}" aria-label="音量"></label>
          <label class="setting">字号<select id="font-size" aria-label="字号"><option value="28" ${state.font===28?'selected':''}>标准 28</option><option value="32" ${state.font===32?'selected':''}>大字 32</option></select></label>
          <label class="setting">息屏时间<select id="idle" aria-label="息屏时间">${[['60','1 分钟'],['120','2 分钟'],['300','5 分钟'],['0','常亮']].map(([v,l])=>`<option value="${v}" ${state.idle===v?'selected':''}>${l}</option>`).join('')}</select></label>
        </div><div class="settings-group"><h2>连接</h2><button class="setting-link" data-route="wifi"><span>Wi-Fi</span><span>${state.offline?'未连接':'已连接'} ›</span></button><div class="setting"><span>USB 键盘</span><span>${state.usb?'已接入':'未接入'}</span></div></div>
        <div class="settings-group"><h2>会话</h2><button class="setting-link danger" id="clear-history"><span>清除对话历史</span><span>›</span></button><p style="color:var(--muted);font-size:17px;line-height:30px">清除后无法恢复，请谨慎操作。</p></div>
        
      </div></section>`;
      case 'wifi': return `<section class="page"><div class="scroll-page"><div class="page-heading" style="margin:0"><span>可用网络</span><button class="quiet" id="rescan">重新扫描</button></div>
        <div class="notice" id="network-notice" role="status">${networkNotice()}</div>
        ${['演示网络 · 花园','演示网络 · 书房','演示网络 · 访客'].map((n,i)=>`<button class="network" data-network="${i}" ${state.networkStatus==='connecting'?'disabled':''}>${icon('wifi')}<span class="network-text"><strong>${n}</strong><small>${state.connected===n && !state.offline?'已连接 · 本地模拟':i===2?'开放网络 · 演示失败与恢复':'需要密码 · 仅用样例密码'}</small></span><span>›</span></button>`).join('')}
        <p style="color:var(--muted);font-size:18px;line-height:32px">这些都是虚构网络。连接流程不访问系统 Wi-Fi。访客网络固定模拟失败，可重试其他网络。</p>
        ${state.networkStatus==='failed'?'<button class="setting-link" id="network-recover">重新选择网络 ›</button>':''}
      </div></section>`;
      case 'menu': return `<section class="page landing"><div class="landing-intro"><span class="eyebrow">HELLO, LET’S CONNECT</span><h2>沟通，<br>从一句话开始。</h2><p>说出来、写下来，或者慢慢看。</p></div>
        <button class="feature feature-talk" data-route="home"><div class="feature-top"><span class="feature-icon">${icon('home')}</span><span class="feature-arrow">↗</span></div><div class="feature-copy"><strong>语音对话</strong><span>说话或打字，即刻交流</span></div><div class="sound-bars" aria-hidden="true">${[12,24,38,20,48,30,56,36,20,42,28,14].map(h=>`<i style="height:${h}px"></i>`).join('')}</div></button>
        <div class="feature-grid"><button class="feature feature-transcript" data-route="transcript"><span class="feature-icon">${icon('transcript')}</span><strong>独立转写</strong><span>把听见的，变成文字</span><span class="mini-arrow">↗</span></button><button class="feature feature-camera" data-route="camera"><span class="feature-icon">${icon('camera')}</span><strong>相机预览</strong><span>用画面辅助表达</span><span class="mini-arrow">↗</span></button></div>
        <div class="landing-tip"><span class="tip-icon">${icon('keyboard')}</span><div><strong>也可以用键盘交流</strong><span>接入 USB 键盘，直接输入</span></div></div></section>`;

    }
  }
  function messagesMarkup() {
    return state.messages.length ? state.messages.map(m=>`<article class="message ${m.who==='user'?'mine':''}"><span class="speaker">${m.who==='user'?'你':'灵译通'}</span>${esc(m.text)}</article>`).join('') : '<div class="notice">还没有对话，按住说话或输入一句话开始。</div>';
  }
  function transcriptMarkup() {
    return state.transcript.length ? state.transcript.map((t,i)=>`<section class="segment ${state.recording && i===state.transcript.length-1?'revising':''}"><div class="segment-meta"><span>${String(i+1).padStart(2,'0')}</span><span>${state.recording && i===state.transcript.length-1?'正在修订':'转写内容'}</span></div><p>${esc(t)}</p></section>`).join('') : '<div class="welcome"><div class="eyebrow">新的一页</div><h2>准备好，就开始。</h2><p>录音或输入后，文字会出现在这里。</p></div>';
  }
  const candidateSets = [['你好','您好','你们'],['拟好','霓虹','泥好']];
  function composerMarkup() {
    return `<div class="composer"><div class="candidates" ${!state.pinyin || state.language!=='zh'?'hidden':''} aria-label="中文候选"><div id="candidate-items" style="display:contents">${candidateSets[state.candidatePage].map(t=>`<button data-candidate="${t}">${t}</button>`).join('')}</div><button class="candidate-page" id="candidate-prev" aria-label="上一页候选">‹</button><button class="candidate-page" id="candidate-next" aria-label="下一页候选">›</button></div>
      <div class="input-row"><button class="icon quiet" id="toggle-keyboard" aria-label="${state.keyboard?'收起':'展开'}软键盘" aria-expanded="${state.keyboard}">${icon('keyboard')}</button><textarea class="draft" id="draft" rows="1" aria-label="文字输入" placeholder="输入文字…">${esc(state.drafts[state.route])}</textarea><button class="primary" id="send" aria-label="发送文字">${icon('send')}</button></div>
      <div class="input-meta"><span id="input-status">${state.usb?'USB 键盘已接入':state.keyboard?'中文输入':'也可以点击键盘输入'}</span><span>${state.language==='zh'?'拼':'E'}</span></div>
      <div class="keyboard" ${state.keyboard?'':'hidden'}><div class="keyboard-head"><button id="language">${state.language==='zh'?'拼 / E':'E / 拼'}</button><span class="composing">${esc(state.pinyin) || (state.language==='zh'?'输入 nihao 试试':'English')}</span><button class="quiet" id="hide-keyboard">收起</button></div>
      ${['qwertyuiop','asdfghjkl','zxcvbnm'].map(keys=>`<div class="key-row">${keys.split('').map(k=>`<button data-key="${k}">${k}</button>`).join('')}</div>`).join('')}
      <div class="key-row"><button data-key="backspace" aria-label="退格">⌫</button><button data-key="space" class="space">空格</button><button data-key="。">。</button><button data-key="enter">确定</button></div></div></div>`;
  }
  function bindCommon() {
    device.querySelectorAll('[data-route]').forEach(b=>b.onclick=()=>navigate(b.dataset.route));
  }
  function navigate(route) {
    if(state.modal) return;
    saveScroll();
    if(state.recording) stopRecording();
    if(state.holding) {state.holding=false;}
    state.route=route; state.keyboard=false; state.pinyin=''; state.feedback=''; state.error=false;
    history.replaceState(null,'',`${location.pathname}${location.search}#${route}`);
    render();
  }
  function saveScroll() {
    if(state.route==='transcript' && $('.reader')) state.scrollTop=$('.reader').scrollTop;
    if(state.route==='home' && $('.reader')) state.homeScroll=$('.reader').scrollTop;
  }
  function bindPage() {
    const r=state.route;
    if(r==='home' || r==='transcript') {
      $('#draft').oninput=e=>{state.drafts[r]=e.target.value;};
      $('#draft').onkeydown=e=>{
        if(e.key==='Enter' && !e.shiftKey && !e.isComposing) {e.preventDefault();sendText();}
      };
      $('#send').onclick=sendText;
      $('#toggle-keyboard').onclick=()=>setKeyboard(!state.keyboard);
      $('#hide-keyboard').onclick=()=>setKeyboard(false);
      $('#language').onclick=()=>{saveScroll();state.language=state.language==='zh'?'en':'zh';state.pinyin='';render();};
      device.querySelectorAll('[data-key]').forEach(b=>b.onclick=()=>key(b.dataset.key));
      bindCandidates();
      if(state.recording){$('#send').disabled=true;$('#draft').disabled=true;$('#toggle-keyboard').disabled=true;}
      $('.reader').onscroll=()=>{
        if(state.changingReader || (state.expectedScroll !== undefined && Math.abs($('.reader').scrollTop-state.expectedScroll)<1)) {state.expectedScroll=undefined;return;}
        state.expectedScroll=undefined;
        if(r==='transcript') {
          state.scrollTop=$('.reader').scrollTop;
          state.follow=atBottom();updateReadingBar();
        } else state.homeScroll=$('.reader').scrollTop;
      };
    }
    if(r==='transcript') {
      $('#record').onclick=()=>state.recording?stopRecording():startRecording();
      $('#latest').onclick=()=>{state.follow=true;lockReader(()=>$('.reader').scrollTop=$('.reader').scrollHeight);state.scrollTop=$('.reader').scrollTop;updateReadingBar();};
      $('#clear-transcript').onclick=()=>{if(!state.recording) openConfirm('transcript');};
    }
    if(r==='home') {
      if($('#interrupt')) $('#interrupt').onclick=interruptReply;
      const hold=$('#hold-talk');
      if(hold) {
        hold.onpointerdown=e=>{if(e.button!==0 || state.modal) return;e.preventDefault();hold.setPointerCapture(e.pointerId);startHold();};
        hold.onpointerup=e=>{if(!state.holding)return;e.preventDefault();finishHold();};
        hold.onpointercancel=()=>cancelHold();
        hold.onkeydown=e=>{if((e.key===' ' || e.key==='Enter') && !e.repeat){e.preventDefault();startHold();}};
        hold.onkeyup=e=>{if(e.key===' ' || e.key==='Enter'){e.preventDefault();finishHold();}};
      }
    }
    if(r==='settings') {
      for(const name of ['brightness','volume']) $('#'+name).oninput=e=>{state[name]=Number(e.target.value);$('#'+name+'-output').value=state[name]+'%';};
      $('#font-size').onchange=e=>{state.font=Number(e.target.value);device.style.setProperty('--body-size',state.font+'px');};
      $('#idle').onchange=e=>state.idle=e.target.value;
      $('#clear-history').onclick=()=>openConfirm('history');
    }
    if(r==='wifi') {
      device.querySelectorAll('[data-network]').forEach(b=>b.onclick=()=>chooseNetwork(Number(b.dataset.network)));
      $('#rescan').onclick=()=>{state.networkStatus=state.connected && !state.offline?'connected':'ready';render();};
      if($('#network-recover')) $('#network-recover').onclick=()=>{state.networkStatus='ready';render();};
    }
    if(r==='camera') {
      $('#pause-camera').onclick=()=>{state.cameraPaused=!state.cameraPaused;render();};
      $('#camera-guide').onclick=()=>{state.cameraGrid=!state.cameraGrid;render();};
      $('.camera-subject').hidden=!state.cameraGrid;
    }
  }
  function lockReader(action) {
    state.changingReader=true; action();
    state.expectedScroll=$('.reader')?.scrollTop;
    state.changingReader=false;
  }
  function atBottom() {
    const el=$('.reader'); return !el || el.scrollHeight-el.clientHeight-el.scrollTop<=32;
  }
  function updateReadingBar() {
    if(state.route!=='transcript') return;
    const remain=!atBottom();
    $('#reading-state').textContent=state.follow?'自动跟随':remain?'正在阅读上文':'已到最新';
    $('#latest').hidden=state.follow || !remain;
  }
  function refreshTranscript() {
    if(state.route!=='transcript') return;
    const el=$('.reader'), top=el.scrollTop;
    lockReader(()=>{el.innerHTML=transcriptMarkup();el.scrollTop=state.follow?el.scrollHeight:Math.min(top,Math.max(0,el.scrollHeight-el.clientHeight));});
    state.scrollTop=el.scrollTop;updateReadingBar();
  }
  function setKeyboard(open) {
    saveScroll();
    state.keyboard=open;state.pinyin='';render();
    if(open) $('#draft').focus({preventScroll:true});
  }
  function key(k) {
    saveScroll();
    const route=state.route;
    if(k==='backspace') {if(state.pinyin)state.pinyin=state.pinyin.slice(0,-1);else state.drafts[route]=Array.from(state.drafts[route]).slice(0,-1).join('');}
    else if(k==='space' || k==='enter') {if(state.pinyin && state.language==='zh'){chooseCandidate(candidateSets[state.candidatePage][0]);return;}state.drafts[route]+=k==='space'?' ':'';}
    else if(state.language==='zh' && /^[a-z]$/.test(k)) {state.pinyin+=k;state.candidatePage=0;}
    else state.drafts[route]+=k;
    $('#draft').value=state.drafts[route];
    $('.composing').textContent=state.pinyin || (state.language==='zh'?'输入 nihao 试试':'English');
    $('.candidates').hidden=!state.pinyin || state.language!=='zh';
    refreshCandidates();
    updateInputLayout();
  }
  function updateInputLayout() {
    const el=$('.reader'),top=state.route==='transcript'?state.scrollTop:state.homeScroll;
    lockReader(()=>{
      device.classList.toggle('candidate-open',!!state.pinyin && state.language==='zh');
      el.scrollTop=state.route==='transcript' && state.follow?el.scrollHeight:top;
    });
    updateReadingBar();
  }
  function refreshCandidates() {
    $('#candidate-items').innerHTML=candidateSets[state.candidatePage].map(t=>`<button data-candidate="${t}">${t}</button>`).join('');bindCandidates();
  }
  function bindCandidates() {
    device.querySelectorAll('[data-candidate]').forEach(b=>b.onclick=()=>chooseCandidate(b.dataset.candidate));
    $('#candidate-prev').onclick=()=>{state.candidatePage=(state.candidatePage+1)%2;refreshCandidates();};
    $('#candidate-next').onclick=()=>{state.candidatePage=(state.candidatePage+1)%2;refreshCandidates();};
  }
  function chooseCandidate(text) {
    saveScroll();
    state.drafts[state.route]+=text;state.pinyin='';$('#draft').value=state.drafts[state.route];$('.candidates').hidden=true;$('.composing').textContent='输入 nihao 试试';
    updateInputLayout();
  }
  function feedback(text,error=false) {
    state.feedback=text;state.error=error;
    const el=$('.feedback');if(el){el.textContent=text;el.classList.toggle('error',error);}
  }
  function sendText() {
    if(state.modal || state.recording || state.replying) {feedback('请先停止当前任务，再发送文字。');return;}
    const text=state.drafts[state.route].trim();
    if(!text){feedback('请先输入一句话。');return;}
    if(state.offline || state.failNext){state.failNext=false;$('#failure-demo').checked=false;feedback('模拟发送失败，文字已保留。恢复连接后可重试。',true);return;}
    state.drafts[state.route]='';state.pinyin='';$('#draft').value='';$('.candidates').hidden=true;
    updateInputLayout();
    if(state.route==='transcript'){state.transcript.push(text);refreshTranscript();feedback('已加入当前转写 · 本地模拟');}
    else {state.messages.push({who:'user',text});state.homeScroll=Number.MAX_SAFE_INTEGER;beginReply();}
  }
  function beginReply() {
    state.replying=true;state.feedback='正在回复 · 本地模拟';state.error=false;render();
    clearTimeout(state.replyTimer);
    state.replyTimer=setTimeout(()=>{
      state.messages.push({who:'assistant',text:'收到，我们可以按这个安排继续。需要调整时，随时告诉我。'});
      state.replying=false;state.feedback='回复完成 · 本地模拟';
      if(state.route==='home'){state.homeScroll=Number.MAX_SAFE_INTEGER;saveHomeRender();}
    },3600);
  }
  function saveHomeRender() {render();}
  function interruptReply() {
    clearTimeout(state.replyTimer);state.replying=false;state.feedback='回复已打断，可以继续说话。';render();
  }
  function startHold() {
    if(state.modal || state.replying || state.holding)return;
    state.holding=true;$('#hold-talk span').textContent='正在聆听 · 松开发送';feedback('正在录音 · 仅模拟，不使用麦克风');
  }
  function finishHold() {
    if(!state.holding)return;state.holding=false;
    if(state.offline || state.failNext){state.drafts.home='我们稍后在花园入口见面。';state.failNext=false;$('#failure-demo').checked=false;state.feedback='模拟发送失败，识别文字保留在输入框。';state.error=true;render();return;}
    state.messages.push({who:'user',text:'我们稍后在花园入口见面。'});state.homeScroll=Number.MAX_SAFE_INTEGER;beginReply();
  }
  function cancelHold(){state.holding=false;if($('#hold-talk span'))$('#hold-talk span').textContent='按住说话';feedback('已取消此次模拟录音');}
  const revisions = ['下一站是花园。','下一站是花园入口，请在入口附近稍候，我们会沿着有遮雨棚的步道过去集合。','下一站是花园入口，请稍候。','下一站是花园入口，请在入口附近稍候。我们会沿着步道一起过去，到了以后再确认下一步安排。'];
  function startRecording() {
    if(state.modal || state.recording)return;
    if(state.offline){feedback('模拟离线，无法开始在线转写。可先输入文字。',true);return;}
    saveScroll();state.recording=true;state.revision=0;state.transcript.push(revisions[0]);state.feedback='模拟识别中 · 清空暂不可用';state.error=false;render();
    state.timer=setInterval(()=>{
      if(!state.recording)return;
      state.revision++;
      if(state.revision%4===0)state.transcript.push(revisions[0]);
      else state.transcript[state.transcript.length-1]=revisions[state.revision%4];
      refreshTranscript();
    },1800);
    $('#send').disabled=true;$('#draft').disabled=true;
  }
  function stopRecording() {
    clearInterval(state.timer);state.timer=null;state.recording=false;
    if(state.route==='transcript'){saveScroll();state.feedback='已停止 · 可以继续输入或清空';state.error=false;render();}
  }
  function openConfirm(kind) {
    saveScroll();state.modal={kind};renderModal();
  }
  function renderModal() {
    const kind=state.modal.kind;
    const wifi=kind==='wifi';
    $('#modal-root').innerHTML=`<div class="modal-shade"><section class="dialog" role="dialog" aria-modal="true" aria-labelledby="dialog-title">
      <h2 id="dialog-title">${wifi?'连接演示网络':kind==='history'?'清除对话历史？':'清空当前转写？'}</h2>
      <p>${wifi?esc(state.pendingNetwork)+'<br>仅输入虚构密码进行演示。':kind==='history'?'清除本次演示对话，操作后无法撤销。':'清空后无法恢复。取消可继续在原位置阅读。'}</p>
      ${wifi?'<label><span style="font-size:18px">演示密码（请勿输入真实密码）</span><input id="demo-password" type="text" value="demo1234" aria-label="演示密码" autocomplete="off"></label>':''}
      <div class="actions"><button id="cancel-modal">取消</button><button class="${wifi?'primary':'danger primary'}" id="confirm-modal">${wifi?'连接':'确认清空'}</button></div>
    </section></div>`;
    $('#workspace').inert=true;
    device.querySelectorAll('.nav button,.header button').forEach(b=>b.disabled=true);
    $('#cancel-modal').onclick=closeModal;
    $('#confirm-modal').onclick=()=>{
      if(wifi){const val=$('#demo-password').value;const ok=val==='demo1234';closeModal();connectNetwork(state.pendingNetwork,ok);return;}
      if(kind==='transcript' && state.recording)return;
      if(kind==='history'){state.messages=[];state.homeScroll=0;}
      else{state.transcript=[];state.follow=true;state.scrollTop=0;}
      closeModal();feedback(kind==='history'?'演示对话已清除。':'已清空，可以继续录音或输入。');
    };
    $('#cancel-modal').focus({preventScroll:true});
  }
  function closeModal() {state.modal=null;render();}
  document.addEventListener('keydown',e=>{
    if(!state.modal)return;
    if(e.key==='Escape'){e.preventDefault();closeModal();return;}
    if(e.key==='Tab') {
      const controls=Array.from($('.dialog').querySelectorAll('button,input'));
      const first=controls[0],last=controls[controls.length-1];
      if(e.shiftKey && document.activeElement===first){e.preventDefault();last.focus();}
      else if(!e.shiftKey && document.activeElement===last){e.preventDefault();first.focus();}
    }
  });
  function chooseNetwork(index) {
    const names=['演示网络 · 花园','演示网络 · 书房','演示网络 · 访客'];
    state.pendingNetwork=names[index];
    if(index===2){connectNetwork(names[index],false);return;}
    state.modal={kind:'wifi'};renderModal();
  }
  function connectNetwork(name,ok) {
    state.networkStatus='connecting';state.connected='';state.offline=true;$('#offline-demo').checked=true;render();
    clearTimeout(state.networkTimer);
    state.networkTimer=setTimeout(()=>{
      if(ok){state.connected=name;state.offline=false;state.networkStatus='connected';$('#offline-demo').checked=false;}
      else {state.networkStatus='failed';state.offline=true;}
      if(state.route==='wifi')render();
    },1500);
  }
  function networkNotice() {
    if(state.networkStatus==='connecting')return `正在连接 ${esc(state.pendingNetwork)}…<br>本地模拟，请稍候。`;
    if(state.networkStatus==='failed')return '连接失败（模拟）。请重新选择网络，或使用样例密码 demo1234 重试。';
    if(state.offline)return '当前离线。选择一个演示网络进行连接。';
    if(state.connected)return `已连接 ${esc(state.connected)}。<br>仅为演示状态，未修改系统 Wi-Fi。`;
    return '扫描完成，选择一个网络继续。';
  }
  $('#route-picker').onchange=e=>navigate(e.target.value);
  $('#usb-demo').onchange=e=>{saveScroll();state.usb=e.target.checked;if(state.usb){state.keyboard=false;state.pinyin='';}render();};
  $('#offline-demo').onchange=e=>{saveScroll();state.offline=e.target.checked;if(state.offline && state.recording)stopRecording();render();};
  $('#failure-demo').onchange=e=>state.failNext=e.target.checked;
  $('#revision-demo').onclick=()=>{
    if(state.route!=='transcript')navigate('transcript');
    if(!state.recording)startRecording();
    else {state.revision++;state.transcript[state.transcript.length-1]=revisions[state.revision%4];refreshTranscript();}
  };
  $('#long-demo').onclick=()=>{
    if(state.recording)stopRecording();
    if(state.route!=='transcript')navigate('transcript');
    state.transcript=sampleTranscript.concat(sampleTranscript);state.follow=true;state.scrollTop=0;state.feedback='长文样例已载入，可上翻后开始模拟转写。';render();
  };
  function scale() {
    const wrap=$('.device-wrap');
    const width=embedded ? innerWidth : innerWidth<=700 ? innerWidth-24 : Math.min(600,innerWidth-(innerWidth<=1000?350:434));
    const height=embedded ? innerHeight : innerWidth<=700 ? 1024 : innerHeight-64;
    const factor=Math.min(1,width/600,height/1024);
    device.style.transform=`scale(${factor})`;wrap.style.width=600*factor+'px';wrap.style.height=1024*factor+'px';
  }
  addEventListener('resize',scale);
  addEventListener('blur',()=>{if(state.holding)cancelHold();});
  // Shareable local design-review states; all content remains fictional and in memory.
  const scenario=query.get('scenario');
  if(state.route==='transcript' && scenario==='reading') {
    state.follow=false;state.scrollTop=220;
  }
  if((state.route==='transcript'||state.route==='home') && scenario==='keyboard') {
    state.keyboard=true;state.pinyin='nihao';
  }
  if(state.route==='transcript' && scenario==='confirm') state.modal={kind:'transcript'};
  render();
})();
