(() => {
  'use strict';
  const scheme='sage';
  const info={name:'青禾',latest:'回到最新 ↓'};
  const query = new URLSearchParams(location.search);
  const embedded = query.get('embed') === '1';
  const validRoutes = ['home','transcript','camera','settings','wifi','menu','help','about'];
  const initialRoute = location.hash.slice(1) || query.get('page') || 'menu';
  const sampleTranscript = [
    '今天的安排从这里开始。我们先确认集合地点，再讨论出发时间。',
    '请慢一点说，我正在看屏幕上的文字。如果有不清楚的地方，我会用键盘补充。',
    '沿着步道向前走，经过花园后在路口左转。那边有一块清楚的指示牌。',
    '午后可能会下小雨，出发时记得带伞。我们可以选择有遮雨棚的路线。',
    '我们会在花园入口等你，不用着急。如果需要调整时间，可以先用文字告诉我，看到以后我会回复，再一起确认接下来的安排。',
    '到了入口以后先稍作休息，再一起决定接下来的安排。'
  ];
  const state = {
    route: validRoutes.includes(initialRoute) ? initialRoute : 'home',
    drafts:{home:'',transcript:''}, language:'zh', pinyin:'', candidatePage:0, keyboard:false, usb:false,
    offline:false, failNext:false, messages:[
      {who:'assistant',text:'你好，今天想聊些什么？'},
      {who:'user',text:'附近有没有可以休息的地方？'},
      {who:'assistant',text:'前面右转有一处休息区。沿着步道走，很快就能看到。'}
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
  const title = {home:'语音对话',transcript:'独立转写',camera:'相机预览',settings:'设置',wifi:'Wi-Fi',menu:'灵译通',help:'使用帮助',about:'关于灵译通'};
  document.body.classList.toggle('embed',embedded);
  document.body.innerHTML = `
    <div class="studio">
      <aside class="notes">
        <a class="studio-brand" href="../p4-ui-fusions-selected/index.html">← 返回四套方案</a>
        <div class="project-title"><span>QINGHE / COMPLETE EXPERIENCE</span><h1>青禾</h1><div class="design-tag">SAGE · QUIET · CLEAR</div><p>慢慢说，我在听。</p></div>
        <p class="description">从点阵开机到一句回应，让每一次切换都有来处。沿用青禾布局，以短纸幕衔接页面，用局部动效说明状态。</p>
        <div class="review-actions"><button id="replay-boot">重看开机</button><button id="sleep-demo">息屏 / 唤醒</button><button class="wide" id="welcome-demo">查看欢迎状态</button></div>
        <label class="scenario-label">启动演示情景<select id="boot-scenario" aria-label="启动演示情景"><option value="normal">正常准备</option><option value="offline">网络未连接</option><option value="delayed">文字资源较慢</option></select></label>
        <fieldset><legend>页面与状态</legend>
          <label>页面 <select id="route-picker" aria-label="切换演示页面"><option value="menu">首页</option><option value="home">语音对话</option><option value="transcript">独立转写</option><option value="camera">相机预览</option><option value="settings">设置</option><option value="wifi">Wi-Fi</option><option value="help">使用帮助</option><option value="about">关于灵译通</option></select></label>
          <div class="checks"><label><input id="reduced-demo" type="checkbox">减少动态效果</label><label><input id="usb-demo" type="checkbox">USB 键盘接入</label><label><input id="offline-demo" type="checkbox">模拟离线</label><label><input id="failure-demo" type="checkbox">下一次发送失败</label></div>
          <button id="revision-demo" class="full">转写 / 修订一句</button><button id="long-demo" class="full">载入长文，体验上翻</button>
        </fieldset>
        <p class="motion-status" id="review-status" role="status">完整交互原型 · 启动流程为加速演示</p>
        <div class="studio-note"><b>600 × 1024 · 为 P4 控制动效成本</b><p>实色纸幕、局部点阵，空闲不动。所有声音、网络及相机均为模拟，无内容保存。</p></div>
        <p class="studio-link"><a href="storyboard.html">全流程设计</a><a href="P4-MOTION-SPEC.md">P4 落地规范</a></p>
      </aside>
      <div class="device-wrap"><section class="device" id="device" aria-label="600乘1024 灵译通融合界面" data-scheme="${scheme}"></section></div>
    </div>`;
  const device = $('#device');
  const motion=window.QingheMotion.attach(device);
  const startup={active:false,welcome:false,sleep:false,timers:[],phase:0,scenario:'normal'};
  if(query.get('reduced')==='1')motion.setReduced(true);
  function render() {
    const r = state.route;
    device.dataset.page=r;
    const subtitle = {menu:'沟通，按你的方式',home:'说话 / 文字',transcript:'当前会话 · 不保存',camera:'取景辅助',settings:'屏幕、声音与连接',wifi:'选择网络',help:'按你的方式交流',about:'让表达更自由'}[r];
    device.style.setProperty('--body-size',`${state.font}px`);
    device.classList.toggle('keyboard-open',state.keyboard);
    device.classList.toggle('candidate-open',!!state.pinyin && state.language==='zh');
    device.innerHTML = `
      <div class="status"><span class="wordmark">LINGYITONG <span>/ 灵译通</span></span><span class="signal"><i class="connection-dot ${state.offline?'offline':''}"></i><span id="connection-indicator">${state.offline?'离线':'已就绪'}</span></span></div>
      <header class="header">${r==='menu'?`<span class="brand-mark">${brandMark()}</span>`:`<button class="icon quiet back" data-route="${['wifi','help','about'].includes(r)?'settings':'menu'}" aria-label="${['wifi','help','about'].includes(r)?'返回设置':'返回首页'}">${icon('back')}</button>`}<div class="heading"><h1>${title[r]}</h1><div class="subtitle">${subtitle}</div></div>${!['settings','wifi','help','about'].includes(r)?`<button class="icon quiet" data-route="settings" aria-label="打开设置">${icon('settings')}</button>`:''}</header>
      <div class="shell"><main class="workspace" id="workspace">${pageMarkup()}</main></div>
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
    scale();motion.restore();syncVisualState();
    if(startup.active||startup.welcome){$('#workspace').inert=true;$('.header').inert=true;}
  }
  function pageMarkup() {
    switch(state.route) {
      case 'home': return `<section class="page">${offlineBanner()}<div class="voice-heading"><span class="voice-state"><span class="tiny-wave" aria-hidden="true">${'<i></i>'.repeat(5)}</span><span id="voice-label">${state.replying?'正在回应':'语音已就绪'}</span></span><span class="mode-tag">声音 / 文字</span></div><div class="reader" id="chat-reader" tabindex="0" aria-label="对话记录">
        <div class="conversation-start"><span></span>今天 · 一次轻松的交流<span></span></div>
        <div id="messages">${messagesMarkup()}</div></div><div class="feedback ${state.error?'error':''}" role="status">${esc(state.feedback)}</div>
        <div class="toolbar">${state.replying?`<button class="record primary" id="interrupt">${icon('stop')}打断回复</button>`:`<button class="record primary" id="hold-talk" aria-label="按住说话，松开发送">${icon('mic')}<span>按住说话</span></button>`}</div>${composerMarkup()}</section>`;
      case 'transcript': return `<section class="page">${offlineBanner()}<div class="page-heading"><span>当前会话</span><span id="record-status">${state.recording?'● 正在聆听':'等待开始'}</span></div>
        <div class="reader transcript" id="transcript-reader" tabindex="0" aria-label="转写正文">${transcriptMarkup()}</div>
        <div class="readingbar"><span id="reading-state">自动跟随</span><button id="latest" hidden>${info.latest}</button></div>
        <div class="feedback ${state.error?'error':''}" role="status">${esc(state.feedback)}</div>
        <div class="toolbar"><button class="record ${state.recording?'':'primary'}" id="record" aria-pressed="${state.recording}">${icon(state.recording?'stop':'mic')}<span>${state.recording?'停止转写':'开始转写'}</span></button><button class="quiet" id="clear-transcript" ${state.recording?'disabled':''}>清空</button></div>${composerMarkup()}</section>`;
      case 'camera': return `<section class="page"><div class="page-heading"><span>预览画面</span><span>模拟</span></div>
        <div class="camera-scene ${state.cameraPaused?'paused':''}"><div class="camera-overlay"><span>${state.cameraPaused?'已暂停':'预览中'}</span><span>取景辅助</span></div><svg class="camera-drawing" viewBox="0 0 500 650" aria-hidden="true"><path d="M40 80h45M40 80v45m420-45h-45m45 0v45M40 570h45m-45 0v-45m420 45h-45m45 0v-45"/><circle cx="250" cy="230" r="57"/><path d="M145 490v-74a105 105 0 0 1 210 0v74m-210-40-46-87m256 87 46-87M105 360l-13-44m18 42 1-52m7 48 9-40m267 46 13-44m-18 42-1-52m-7 48-9-40"/></svg><div class="camera-subject"></div><div class="camera-caption">${state.cameraPaused?'预览已暂停':'请将人物置于画面中央'}</div></div>
        <div class="scroll-page" style="flex:0 0 auto"><div class="notice">相机示意画面 · 当前仅演示预览与取景，不包含手语识别。</div><div class="toolbar" style="margin:0"><button class="primary" id="pause-camera" style="flex:1">${state.cameraPaused?'继续预览':'暂停预览'}</button><button id="camera-guide">${state.cameraGrid?'隐藏辅助线':'显示辅助线'}</button></div></div></section>`;
      case 'settings': return `<section class="page"><div class="scroll-page">
        <div class="settings-group"><h2>屏幕与声音</h2>
          <label class="setting vertical"> <span>亮度 <output id="brightness-output">${state.brightness}%</output></span><input id="brightness" type="range" min="20" max="100" value="${state.brightness}" aria-label="亮度"></label>
          <label class="setting vertical"><span>音量 <output id="volume-output">${state.volume}%</output></span><input id="volume" type="range" min="0" max="100" value="${state.volume}" aria-label="音量"></label>
          <label class="setting">字号<select id="font-size" aria-label="字号"><option value="28" ${state.font===28?'selected':''}>标准 28</option></select></label>
          <label class="setting">息屏时间<select id="idle" aria-label="息屏时间">${[['60','1 分钟'],['120','2 分钟'],['300','5 分钟'],['0','常亮']].map(([v,l])=>`<option value="${v}" ${state.idle===v?'selected':''}>${l}</option>`).join('')}</select></label>
        </div><div class="settings-group"><h2>连接</h2><button class="setting-link" data-route="wifi"><span>Wi-Fi</span><span>${state.offline?'未连接':'已连接'} ›</span></button><div class="setting"><span>USB 键盘</span><span>${state.usb?'已接入':'未接入'}</span></div></div>
        <div class="settings-group"><h2>了解与帮助</h2><button class="setting-link" data-route="help"><span>使用帮助</span><span>›</span></button><button class="setting-link" data-route="about"><span>关于灵译通</span><span>›</span></button></div><div class="settings-group"><h2>会话</h2><button class="setting-link danger" id="clear-history"><span>清除对话历史</span><span>›</span></button><p style="color:var(--muted);font-size:17px;line-height:30px">清除后无法恢复，请谨慎操作。</p></div>
        
      </div></section>`;
      case 'wifi': return `<section class="page"><div class="scroll-page"><div class="page-heading" style="margin:0"><span>可用网络</span><button class="quiet" id="rescan">重新扫描</button></div>
        <div class="notice" id="network-notice" role="status">${networkNotice()}</div>
        ${['演示网络 · 花园','演示网络 · 书房','演示网络 · 访客'].map((n,i)=>`<button class="network" data-network="${i}" ${state.networkStatus==='connecting'?'disabled':''}>${icon('wifi')}<span class="network-text"><strong>${n}</strong><small>${state.connected===n && !state.offline?'已连接 · 本地模拟':i===2?'开放网络 · 演示失败与恢复':'需要密码 · 仅用样例密码'}</small></span><span>›</span></button>`).join('')}
        <p style="color:var(--muted);font-size:18px;line-height:32px">这些都是虚构网络。连接流程不访问系统 Wi-Fi。访客网络固定模拟失败，可重试其他网络。</p>
        ${state.networkStatus==='failed'?'<button class="setting-link" id="network-recover">重新选择网络 ›</button>':''}
      </div></section>`;
      case 'help': return `<section class="page"><div class="scroll-page support-page"><div class="notice">不必急着说完。声音与文字，可以随时换一种方式。</div>${[['01','语音对话','按住说话，松开发送。回复中可点“打断回复”，接着说下一句。'],['02','独立转写','点击开始转写。往上翻阅时保留当前位置，点击“回到最新”继续跟随。'],['03','键盘输入','点键盘图标展开软键盘，用顶部“拼 / E”切换。USB 键盘可直接输入。'],['04','清空与取消','清空前会再次确认。取消后，文字与阅读位置都会保留。']].map(([n,t,p])=>`<div class="help-step"><span>${n}</span><strong>${t}</strong><p>${p}</p></div>`).join('')}</div></section>`;
      case 'about': return `<section class="page"><div class="scroll-page support-page"><div class="about-logo">${motion.leafSVG(110)}</div><div class="about-intro"><h2>灵译通</h2><p>每一种表达，都被听见。</p></div><div class="settings-group"><div class="setting"><span>语音交流</span><span>说话 / 文字</span></div><div class="setting"><span>独立转写</span><span>从容阅读</span></div><div class="setting"><span>外接键盘</span><span>USB 输入</span></div></div><p>清楚的文字，熟悉的声音。按你习惯的方式，让沟通继续。</p></div></section>`;
      case 'menu': return landingMarkup();

    }
  }

  function brandMark() { return motion.leafSVG(48); }
  function waveform() {return '<span class="waveform" aria-hidden="true">'+[22,42,64,90,60,38,72,106,76,48,26].map(h=>'<i style="height:'+h+'px"></i>').join('')+'</span>';}
  function featureLink(route,n,label,desc) {return '<button class="entry-link" data-route="'+route+'"><span class="entry-number">'+n+'</span><span class="entry-symbol">'+icon(route==='home'?'mic':route)+'</span><span class="entry-text"><strong>'+label+'</strong><small>'+desc+'</small></span><span class="entry-arrow">↗</span></button>';}
  function offlineBanner(){return state.offline?`<div class="connection-banner"><span>网络未连接，输入的文字会保留</span><button data-route="wifi">连接网络 ↗</button></div>`:'';}
  function landingMarkup(){return `<section class="page landing sage-landing"><div class="home-status"><span class="state-chip"><i></i>${state.offline?'网络未连接':'准备好交流'}</span><button data-route="help" class="help-link">使用帮助 ↗</button></div><div class="sage-greeting"><span class="eyebrow">很高兴见到你</span><h2 class="landing-title">慢慢说，<br>我在听。</h2></div><div class="sage-hero"><span class="ready-pill"><i></i>${state.offline?'也可以先用文字':'每一种表达，都被听见'}</span>${waveform()}<p>说话或打字，按你的方式来。</p><button class="launch" data-route="home">${icon('mic')}<span>开始语音对话</span><span>↗</span></button></div><div class="sage-grid">${featureLink('transcript','02','独立转写','边听，边读')}${featureLink('camera','03','相机预览','让表达更直观')}</div><div class="landing-foot">${icon('keyboard')}<span>也可以接入 USB 键盘，直接输入</span></div></section>`;}

  function messagesMarkup() {
    return state.messages.length ? state.messages.map(m=>`<article class="message ${m.who==='user'?'mine':''}"><span class="speaker">${m.who==='user'?'你':'灵译通'}<i>${m.who==='user'?'YOU':'LINGYITONG'}</i></span><div class="message-text">${esc(m.text)}</div></article>`).join('') : '<div class="notice">还没有对话，按住说话或输入一句话开始。</div>';
  }
  function transcriptMarkup() {
    return state.transcript.length ? state.transcript.map((t,i)=>`<section class="segment ${state.recording && i===state.transcript.length-1?'revising':''}"><div class="segment-meta"><span>${String(i+1).padStart(2,'0')}</span><span>${state.recording && i===state.transcript.length-1?'正在修订':'已转写'}</span></div><p>${esc(t)}</p></section>`).join('') : '<div class="welcome"><div class="eyebrow">新的一页</div><h2>准备好，就开始。</h2><p>录音或输入后，文字会出现在这里。</p></div>';
  }
  function statusNote(text){$('#review-status').textContent=text;}
  function pulseBars(container,n){if(!container)return;container.querySelectorAll('i').forEach((bar,i)=>{bar.style.height=(5+((n+i*2)%5)*5)+'px';});}
  function syncVisualState(){
    const speech=state.route==='home'&&(state.holding||state.replying);
    const label=$('#voice-label');
    if(label)label.textContent=state.offline?'等待联网':state.holding?'正在聆听':state.replying?'正在回应':'语音已就绪';
    if($('#hold-talk'))$('#hold-talk').disabled=state.offline;
    if(speech)motion.loop('speech',125,n=>pulseBars($('.tiny-wave'),n),()=>state.route==='home'&&(state.holding||state.replying));
    else{motion.cancel('speech');$('.tiny-wave')?.querySelectorAll('i').forEach(b=>b.style.height='5px');}
  }
  function clearStartupTimers(){startup.timers.forEach(clearTimeout);startup.timers=[];motion.cancel('boot-dots');motion.cancel('welcome-bars');}
  function later(ms,callback){const id=setTimeout(callback,ms);startup.timers.push(id);return id;}
  function protectUnderlay(value){if($('#workspace'))$('#workspace').inert=value;if($('.header'))$('.header').inert=value;}
  function drawLeafPhase(root,phase){root.querySelectorAll('.leaf-on').forEach(dot=>{dot.classList.toggle('lit',Number(dot.dataset.row)>=8-phase);});}
  function bootMarkup(){return `<div class="boot-screen" role="status" aria-label="启动准备"><div class="boot-wordmark">LINGYITONG</div><div class="boot-emblem">${motion.leafSVG()}</div><h1>灵译通</h1><p class="boot-slogan">每一种表达，都被听见。</p><div class="boot-status"><p id="boot-label">正在点亮屏幕…</p><div class="boot-stages" aria-label="资源准备阶段"><span></span><span></span><span></span></div><div class="boot-stage-labels"><span>显示就绪</span><span>声音与文字</span><span>连接服务</span></div></div><div class="boot-footer">慢慢说，我在听。</div></div>`;}
  async function showWelcome(frozen=false){
    if(state.recording||state.replying||state.holding||motion.busy){statusNote('请先结束当前任务，再查看欢迎状态。');return;}
    clearStartupTimers();startup.active=false;startup.welcome=true;startup.sleep=false;
    state.route='menu';state.keyboard=false;state.pinyin='';render();
    const overlay=document.createElement('div');overlay.className='welcome-screen';overlay.setAttribute('role','status');
    overlay.innerHTML=`<span class="ready-pill"><i></i> 很高兴见到你</span><div class="welcome-bars" aria-hidden="true">${'<i></i>'.repeat(7)}</div><h2>欢迎使用灵译通</h2><p>你可以说话，<br>也可以打字。</p><div class="welcome-caption">正在播放欢迎语</div>`;
    motion.mount(overlay);protectUnderlay(true);device.dataset.experience='welcome';
    motion.loop('welcome-bars',125,n=>pulseBars(overlay.querySelector('.welcome-bars'),n),()=>startup.welcome&&overlay.isConnected);
    statusNote('欢迎状态为本地示意，不播放真实音频。实际固件应由播放完成事件结束。');
    if(!frozen)later(2200,async()=>{
      if(!startup.welcome)return;
      await motion.wipe(()=>{startup.welcome=false;motion.cancel('welcome-bars');motion.unmount();render();device.dataset.experience='ready';});
      statusNote('已进入首页，可从卡片开始体验。原型没有实际音频或设备连接。');
    });
  }
  async function beginBoot(frozen=false){
    if(state.modal||state.recording||state.replying||state.holding||motion.busy){statusNote('请先结束当前任务，再重看开机。');return;}
    clearStartupTimers();startup.active=true;startup.welcome=false;startup.sleep=false;
    startup.scenario=$('#boot-scenario').value;state.offline=startup.scenario==='offline';$('#offline-demo').checked=state.offline;
    state.route='menu';state.keyboard=false;state.pinyin='';render();
    const node=document.createElement('div');node.innerHTML=bootMarkup();const screen=node.firstElementChild;
    motion.mount(screen);protectUnderlay(true);device.dataset.experience='boot';
    const flags=[false,false,false];
    const update=(stage,text)=>{flags[stage]=true;screen.querySelector('#boot-label').textContent=text;screen.querySelectorAll('.boot-stages span').forEach((s,i)=>s.classList.toggle('done',flags[i]));};
    drawLeafPhase(screen,frozen?5:0);
    if(frozen){update(0,'正在准备声音与文字…');statusNote('启动设计状态快照；点“重看开机”可体验完整流程。');return;}
    const start=performance.now();
    motion.loop('boot-dots',125,()=>drawLeafPhase(screen,Math.min(8,Math.floor((performance.now()-start)/220))),()=>startup.active&&screen.isConnected);
    statusNote('加速演示资源事件；真实开机时长取决于字库、音频和网络准备。');
    later(650,()=>update(0,'正在准备声音与文字…'));
    later(1750,()=>{if(startup.scenario!=='delayed')update(1,'正在连接服务…');else screen.querySelector('#boot-label').textContent='文字资源仍在准备，请稍候…';});
    later(2850,()=>{if(startup.scenario!=='delayed')update(2,state.offline?'网络未连接，可以先进入首页':'准备完成，正在进入首页…');});
    const finishAt=startup.scenario==='delayed'?5300:3400;
    if(startup.scenario==='delayed'){later(4500,()=>update(1,'文字资源已就绪，正在连接服务…'));later(4800,()=>update(2,'准备完成，正在进入首页…'));}
    later(finishAt,async()=>{
      if(!startup.active)return;
      // A real port binds this boundary to existing startup flags, never to this demo timer.
      await motion.wipe(()=>{startup.active=false;motion.cancel('boot-dots');motion.unmount();render();device.dataset.experience='ready';});
      if(!state.offline)await showWelcome();else statusNote('离线启动示意：保留文字输入和设置入口，等待联网后再使用在线语音。');
    });
  }
  async function toggleSleep(){
    if(startup.active||startup.welcome||state.recording||state.replying||state.holding||state.modal||motion.busy){statusNote('当前任务进行中，暂不息屏。');return;}
    if(startup.sleep){await wake();return;}
    saveScroll();startup.sleep=true;motion.cancel('speech');
    const screen=document.createElement('button');screen.className='sleep-screen';screen.setAttribute('aria-label','唤醒屏幕');
    screen.onclick=event=>{event.preventDefault();event.stopPropagation();wake();};
    motion.mount(screen);device.dataset.experience='sleep';statusNote('息屏示意：点击黑色屏幕，首次点击仅唤醒，不触发下方按钮。');
  }
  async function wake(){
    if(!startup.sleep||motion.busy)return;
    await motion.wipe(()=>{startup.sleep=false;motion.unmount();render();device.dataset.experience='ready';},-1);
    statusNote('已唤醒，内容和阅读位置保留；首次点击已消耗。');
  }
  function bindExperience(){
    $('#reduced-demo').checked=motion.reduced;
    $('#reduced-demo').onchange=e=>{motion.setReduced(e.target.checked);syncVisualState();};
    $('#replay-boot').onclick=()=>beginBoot();$('#welcome-demo').onclick=()=>showWelcome();$('#sleep-demo').onclick=toggleSleep;
    document.addEventListener('visibilitychange',()=>{if(document.hidden)motion.cancel('speech');else syncVisualState();});
    window.qingheReview={getTrace:()=>motion.trace,getState:()=>({route:state.route,boot:startup.active,welcome:startup.welcome,sleep:startup.sleep,busy:motion.busy,reduced:motion.reduced})};
    const scene=query.get('scene');
    if(scene==='boot')beginBoot(true);
    else if(scene==='welcome')showWelcome(true);
    else if(scene==='sleep')toggleSleep();
    else if(scene==='offline'){state.offline=true;$('#offline-demo').checked=true;render();}
    else if(scene==='listening'){state.route='home';render();startHold();}
    else if(scene==='thinking'){state.route='home';state.replying=true;state.feedback='正在整理回应…';render();}
    else if(!query.has('page')&&!location.hash)beginBoot();
    else device.dataset.experience='ready';
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
  async function navigate(route){
    if(!validRoutes.includes(route))return;
    if(state.modal||motion.busy||startup.active||startup.welcome||startup.sleep){$('#route-picker').value=state.route;return;}
    if(route===state.route)return;
    saveScroll();if(state.recording)stopRecording();if(state.holding)cancelHold();
    const direction=route==='menu'||(route==='settings'&&['wifi','help','about'].includes(state.route))?-1:1;
    await motion.wipe(()=>{state.route=route;state.keyboard=false;state.pinyin='';state.feedback='';state.error=false;history.replaceState(null,'',`${location.pathname}${location.search}#${route}`);render();},direction,route==='camera'?'准备取景':'');
    $('#route-picker').value=state.route;
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
      const activeReader=$('.reader');
      activeReader.onscroll=()=>{
        if(!activeReader.isConnected || state.route!==r) return;
        if(state.changingReader || (state.expectedScroll !== undefined && Math.abs(activeReader.scrollTop-state.expectedScroll)<1)) {state.expectedScroll=undefined;return;}
        state.expectedScroll=undefined;
        if(r==='transcript') {
          state.scrollTop=activeReader.scrollTop;
          state.follow=atBottom();updateReadingBar();
        } else state.homeScroll=activeReader.scrollTop;
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
  async function setKeyboard(open){
    if(state.dockBusy||state.modal||startup.active||startup.welcome||motion.busy)return;
    saveScroll();const route=state.route,top=route==='transcript'?state.scrollTop:state.homeScroll;
    const keep=()=>{if(route!==state.route||!$('.reader'))return;lockReader(()=>{$('.reader').scrollTop=route==='transcript'&&state.follow?$('.reader').scrollHeight:top});updateReadingBar();};
    state.dockBusy=true;
    if(open){state.keyboard=true;state.pinyin='';render();await motion.dock($('.keyboard'),true,keep);if($('#draft'))$('#draft').focus({preventScroll:true});}
    else{const node=$('.keyboard');const done=await motion.dock(node,false,keep);if(done&&state.route===route){state.keyboard=false;state.pinyin='';render();}}
    state.dockBusy=false;
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
      const next=!!state.pinyin&&state.language==='zh',changed=device.classList.contains('candidate-open')!==next;
      const composer=$('.composer'),before=parseFloat(getComputedStyle(composer).marginTop);
      device.classList.toggle('candidate-open',next);
      if(changed){composer.style.marginTop=before+'px';motion.candidate(composer,next,()=>{if(!el.isConnected)return;lockReader(()=>{el.scrollTop=state.route==='transcript'&&state.follow?el.scrollHeight:top});updateReadingBar();});}
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
    if(startup.active||startup.welcome||startup.sleep||motion.busy)return;
    if(state.modal || state.recording || state.replying) {feedback('请先停止当前任务，再发送文字。');return;}
    const text=state.drafts[state.route].trim();
    if(!text){feedback('请先输入一句话。');return;}
    if(state.offline || state.failNext){state.failNext=false;$('#failure-demo').checked=false;feedback('模拟发送失败，文字已保留。恢复连接后可重试。',true);return;}
    state.drafts[state.route]='';state.pinyin='';$('#draft').value='';$('.candidates').hidden=true;
    updateInputLayout();
    if(state.route==='transcript'){state.transcript.push(text);refreshTranscript();feedback('已加入当前转写');}
    else {state.messages.push({who:'user',text});state.homeScroll=Number.MAX_SAFE_INTEGER;beginReply();}
  }
  function beginReply() {
    state.replying=true;state.feedback='正在整理回应…';state.error=false;render();
    clearTimeout(state.replyTimer);
    state.replyTimer=setTimeout(()=>{
      state.messages.push({who:'assistant',text:'收到，我们可以按这个安排继续。需要调整时，随时告诉我。'});
      state.replying=false;state.feedback='回复完成，可以继续交流';
      if(state.route==='home'){state.homeScroll=Number.MAX_SAFE_INTEGER;saveHomeRender();}
    },3600);
  }
  function saveHomeRender() {render();}
  function interruptReply() {
    clearTimeout(state.replyTimer);state.replying=false;state.feedback='回复已打断，可以继续说话。';render();
  }
  function startHold() {
    if(startup.active||startup.welcome||startup.sleep||motion.busy||state.offline||state.modal||state.replying||state.holding)return;
    state.holding=true;$('#hold-talk span').textContent='正在聆听 · 松开发送';feedback('正在聆听，松开后发送');syncVisualState();
  }
  function finishHold() {
    if(!state.holding)return;state.holding=false;
    if(state.offline || state.failNext){state.drafts.home='我们稍后在花园入口见面。';state.failNext=false;$('#failure-demo').checked=false;state.feedback='模拟发送失败，识别文字保留在输入框。';state.error=true;render();return;}
    state.messages.push({who:'user',text:'我们稍后在花园入口见面。'});state.homeScroll=Number.MAX_SAFE_INTEGER;beginReply();
  }
  function cancelHold(){state.holding=false;if($('#hold-talk span'))$('#hold-talk span').textContent='按住说话';feedback('此次录音已取消');syncVisualState();}
  const revisions = ['下一站是花园。','下一站是花园入口，请在入口附近稍候，我们会沿着有遮雨棚的步道过去集合。','下一站是花园入口，请稍候。','下一站是花园入口，请在入口附近稍候。我们会沿着步道一起过去，到了以后再确认下一步安排。'];
  function startRecording() {
    if(startup.active||startup.welcome||startup.sleep||motion.busy||state.modal||state.recording)return;
    if(state.offline){feedback('模拟离线，无法开始在线转写。可先输入文字。',true);return;}
    saveScroll();state.recording=true;state.revision=0;state.transcript.push(revisions[0]);state.feedback='正在转写，停止后可输入或清空';state.error=false;render();
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
      <p>${wifi?esc(state.pendingNetwork)+'<br>仅输入虚构密码进行演示。':kind==='history'?'清除本次演示对话，操作后无法撤销。':'清空后无法恢复。<br>取消后可留在原处继续阅读。'}</p>
      ${wifi?'<label><span style="font-size:18px">演示密码（请勿输入真实密码）</span><input id="demo-password" type="text" value="demo1234" aria-label="演示密码" autocomplete="off"></label>':''}
      <div class="actions"><button id="cancel-modal">取消</button><button class="${wifi?'primary':'danger primary'}" id="confirm-modal">${wifi?'连接':'确认清空'}</button></div>
    </section></div>`;
    $('#workspace').inert=true;
    device.querySelectorAll('.header button').forEach(b=>b.disabled=true);
    motion.reveal($('.dialog'),'modal');
    $('#cancel-modal').onclick=closeModal;
    $('#confirm-modal').onclick=async()=>{
      if(state.modalClosing)return;
      if(wifi){const val=$('#demo-password').value;const ok=val==='demo1234';await closeModal();connectNetwork(state.pendingNetwork,ok);return;}
      if(kind==='transcript' && state.recording)return;
      if(kind==='history'){state.messages=[];state.homeScroll=0;}
      else{state.transcript=[];state.follow=true;state.scrollTop=0;}
      await closeModal();feedback(kind==='history'?'演示对话已清除。':'已清空，可以继续录音或输入。');
    };
    $('#cancel-modal').focus({preventScroll:true});
  }
  async function closeModal(){if(!state.modal||state.modalClosing)return;state.modalClosing=true;device.querySelectorAll('.dialog button').forEach(b=>b.disabled=true);await motion.reveal($('.dialog'),'modal',true);state.modal=null;state.modalClosing=false;render();}
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
  $('#revision-demo').onclick=async()=>{
    if(startup.active||startup.welcome||motion.busy)return;
    if(state.route!=='transcript')await navigate('transcript');
    if(!state.recording)startRecording();
    else {state.revision++;state.transcript[state.transcript.length-1]=revisions[state.revision%4];refreshTranscript();}
  };
  $('#long-demo').onclick=async()=>{
    if(startup.active||startup.welcome||motion.busy)return;
    if(state.recording)stopRecording();
    if(state.route!=='transcript')await navigate('transcript');
    state.transcript=sampleTranscript.concat(sampleTranscript);state.follow=true;state.scrollTop=0;state.feedback='可向上翻阅，或开始转写继续记录。';render();
  };
  function scale() {
    const wrap=$('.device-wrap');
    const viewportWidth=document.documentElement.clientWidth;
    const width=embedded ? innerWidth : viewportWidth<=700 ? viewportWidth-24 : Math.min(600,viewportWidth-(viewportWidth<=1000?350:434));
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
  bindExperience();
})();
