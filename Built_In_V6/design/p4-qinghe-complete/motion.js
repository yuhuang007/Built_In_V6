(() => {
  'use strict';
  // Browser preview of bounded, timer-driven primitives. No external renderer.
  const FRAME_MS=50;
  const leaf=['000010000','000111000','011010110','111010111','011010110','000010000','000010000','001111100','000010000'];
  const leafSVG=(size=180)=>`<svg viewBox="0 0 180 180" width="${size}" height="${size}" aria-hidden="true">${leaf.flatMap((row,y)=>[...row].map((on,x)=>`<circle cx="${18+x*18}" cy="${18+y*18}" r="${on==='1'?4.8:1.4}" class="${on==='1'?'leaf-on':'leaf-grid'}" data-row="${y}"/>`)).join('')}</svg>`;
  function attach(device) {
    const jobs=new Map(), trace=[];
    let pump=null,reduced=matchMedia('(prefers-reduced-motion: reduce)').matches,busy=false,veil=null,overlay=null;
    function record(kind,detail={}){trace.push({kind,...detail});if(trace.length>64)trace.shift();device.dataset.motion=busy?'busy':'idle';}
    function settle(){if(!jobs.size&&pump){clearInterval(pump);pump=null;}}
    function animate(key,duration,step,alive=()=>true) {
      if(jobs.has(key)){jobs.get(key).resolve(false);jobs.delete(key);}
      if(reduced){step(1);return Promise.resolve(true);}
      return new Promise(resolve=>{
        const start=performance.now();step(0);
        jobs.set(key,{start,duration,step,resolve,alive});
        if(!pump)pump=setInterval(()=>{
          for(const [id,job]of [...jobs]){
            if(!job.alive()){jobs.delete(id);job.resolve(false);continue;}
            const t=Math.min(1,(performance.now()-job.start)/job.duration);
            job.step(t<.5?2*t*t:1-Math.pow(-2*t+2,2)/2);
            if(t===1){jobs.delete(id);job.resolve(true);}
          }settle();
        },FRAME_MS);
      });
    }
    function cancelComponents(){for(const [id,j]of [...jobs])if(id!=='route'){jobs.delete(id);j.resolve(false)}settle();}
    function restore(){if(veil)device.append(veil);if(overlay)device.append(overlay);device.inert=busy;device.dataset.motion=busy?'busy':'idle';}
    async function wipe(commit,direction=1,label='') {
      if(busy)return false;
      busy=true;cancelComponents();device.inert=true;
      veil=document.createElement('div');veil.className='page-curtain';veil.setAttribute('aria-hidden','true');
      veil.innerHTML=`<span class="curtain-line"></span><div>${leafSVG(52)}${label?'<span>'+label+'</span>':''}</div><span class="curtain-line"></span>`;
      device.append(veil);record('route-start',{direction});
      try{
        await animate('route',150,p=>veil.style.transform=`translateX(${Math.round(direction*600*(1-p))}px)`);
        await commit();restore();record('route-committed');
        await animate('route',170,p=>veil.style.transform=`translateX(${Math.round(-direction*600*p)}px)`);
        return true;
      } finally {veil?.remove();veil=null;busy=false;device.inert=false;record('route-end');}
    }
    async function reveal(node,kind='panel',closing=false){
      if(!node)return;
      record(kind+'-'+(closing?'out':'in'));
      return animate(kind,kind==='modal'?160:150,p=>{node.style.transform=`translateY(${Math.round((closing?p:1-p)*10)}px)`;node.style.opacity=String(closing?1-p:p);},()=>node.isConnected);
    }
    async function dock(node,opening,keepReading){
      if(!node)return;
      const height=node.getBoundingClientRect().height/(device.getBoundingClientRect().width/600);
      node.style.overflow='hidden';record('keyboard-'+(opening?'in':'out'));
      const done=await animate('dock',180,p=>{node.style.height=(height*(opening?p:1-p))+'px';keepReading();},()=>node.isConnected);
      if(done){node.style.height='';node.style.overflow='';keepReading();}return done;
    }
    async function candidate(composer,opening,keepReading){
      if(!composer)return;
      record('candidate-'+(opening?'in':'out'));
      const from=parseFloat(composer.style.marginTop)||0,to=opening?90:0;
      const done=await animate('candidate',150,p=>{composer.style.marginTop=(from+(to-from)*p)+'px';keepReading();},()=>composer.isConnected);
      if(done){composer.style.marginTop='';keepReading();}
    }
    function mount(node){overlay=node;restore();}
    function unmount(){overlay?.remove();overlay=null;}
    function cancel(key){if(jobs.has(key)){jobs.get(key).resolve(false);jobs.delete(key);settle();}}
    function loop(key,period,step,alive){
      let last=-1;
      if(reduced){step(0);return;}
      return animate(key,86400000,()=>{const n=Math.floor(performance.now()/period);if(n!==last&&!document.hidden){last=n;step(n)}},alive);
    }
    const api={animate,wipe,reveal,dock,candidate,restore,mount,unmount,leafSVG,loop,cancel,
      get busy(){return busy},get reduced(){return reduced},get trace(){return trace.slice()},
      setReduced(value){reduced=!!value;record('motion-preference',{reduced})},
      destroy(){for(const j of jobs.values())j.resolve(false);jobs.clear();settle();unmount();veil?.remove();}
    };
    return api;
  }
  window.QingheMotion={attach,leafSVG,frameInterval:FRAME_MS};
})();
