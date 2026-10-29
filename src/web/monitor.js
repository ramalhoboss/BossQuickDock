/* Session diagnostics: bounded browser memory; no audio or pairing keys collected. */
var BQDMonitor=(function(){
 function num(x){return typeof x==='number'&&isFinite(x)?Math.max(0,x):0;}
 function sample(s,t){var c=s.capture||{},l=s.latency||{};return {at_ms:t,packets:num(s.pkts),dropped:num(s.dropped),queue:num(s.backlog),bitpool:num(s.bitpool),estimate_ms:num(l.estimate_ms),recoveries:num(c.recoveries),overrun:num(c.overrun),capture_state:num(c.state),pcm_age_ms:num(c.age_ms),valid:num(c.valid)};}
 function create(){return {history:[],test:null,last:null,identity:'',lastAt:0};}
 function identity(s){return (s.token||'')+'|'+s.version+'|'+s.device;}
 function start(m,s,now,seconds){if(!s||s.state!=='streaming'||s.paused)throw Error('Start playback before running a test.');if([60,300,600].indexOf(seconds)<0)throw Error('Invalid duration.');var a=sample(s,now);m.test={active:true,started_ms:now,ended_ms:null,target_s:seconds,version:s.version,device:s.device,identity:identity(s),baseline:a,last:a,samples:[],observed_ms:0,gaps:0,max_gap_ms:0,dropped:0,recoveries:0,overrun:0,queue_max:0,queue_sum:0,queue_n:0,reason:'running'};observeTest(m.test,s,a,now);return m.test;}
 function finish(m,now,why){if(m.test&&m.test.active){m.test.active=false;m.test.ended_ms=now;m.test.reason=why||'stopped by user';}return m.test;}
 function observeTest(t,s,a,now){if(!t.active)return;
  if(identity(s)!==t.identity||a.packets<t.last.packets||a.dropped<t.last.dropped||a.recoveries<t.last.recoveries||a.valid<t.last.valid){t.active=false;t.ended_ms=now;t.reason='session changed or counters reset';return;}
  if(s.state!=='streaming'||s.paused){t.active=false;t.ended_ms=now;t.reason='playback interrupted';return;}
  var dt=now-t.last.at_ms;if(dt>0){t.max_gap_ms=Math.max(t.max_gap_ms,dt);if(dt<=3500)t.observed_ms+=dt;else t.gaps++;}
  t.dropped+=a.dropped-t.last.dropped;t.recoveries+=a.recoveries-t.last.recoveries;t.overrun+=Math.max(0,a.overrun-t.last.overrun);
  t.queue_max=Math.max(t.queue_max,a.queue);t.queue_sum+=a.queue;t.queue_n++;t.last=a;
  t.samples.push(a);if(t.samples.length>620)t.samples.shift();
  if(now-t.started_ms>=t.target_s*1000){t.active=false;t.ended_ms=now;t.reason='duration completed';}
 }
 function observe(m,s,now){var a=sample(s,now),id=identity(s);if(m.identity!==id||(m.last&&(a.packets<m.last.packets||a.dropped<m.last.dropped))){m.history=[];}m.identity=id;
  m.history.push(a);if(m.history.length>120)m.history.shift();m.last=a;m.lastAt=now;if(m.test)observeTest(m.test,s,a,now);return a;}
 function tick(m,now){var t=m.test;if(t&&t.active&&now-t.started_ms>=t.target_s*1000)finish(m,t.started_ms+t.target_s*1000,'duration completed');}
 function report(m,now){var t=m.test;if(!t)return null;var elapsed=Math.max(0,((t.active?now:t.ended_ms)-t.started_ms)/1000),coverage=elapsed?Math.min(1,t.observed_ms/(elapsed*1000)):0;
  return {active:t.active,version:t.version,device:t.device,reason:t.reason,target_s:t.target_s,elapsed_s:Math.round(elapsed),observed_s:Math.round(t.observed_ms/1000),coverage_pct:Math.round(coverage*100),observation_gaps:t.gaps+((t.active?now:t.ended_ms)-t.last.at_ms>3500?1:0),max_gap_ms:Math.max(t.max_gap_ms,(t.active?now:t.ended_ms)-t.last.at_ms),packets:Math.max(0,t.last.packets-t.baseline.packets),dropped:t.dropped,drops_per_min:t.observed_ms?Math.round(t.dropped*600000/t.observed_ms)/10:null,recoveries:t.recoveries,overrun:t.overrun,queue_max:t.queue_max,queue_avg:t.queue_n?Math.round(t.queue_sum/t.queue_n*10)/10:0,result:t.active?'running':t.reason!=='duration completed'?'interrupted':coverage<0.9?'insufficient observation':t.dropped||t.recoveries||t.overrun?'events detected':'no drops or recoveries observed',note:'API sampling test; it cannot detect every audible dropout or measure end-to-end latency.'};}
 function clean(v){if(Array.isArray(v))return v.map(clean);if(v&&typeof v==='object'){var o={};Object.keys(v).forEach(function(k){if(!/token|password|secret|pair.*key|link.*key/i.test(k))o[k]=clean(v[k]);});return o;}return v;}
 function recent(m,now){var a=m.history,cut=now-60000,ms=0,drops=0,packets=0,gaps=0,maxq=0;for(var i=1;i<a.length;i++){var p=a[i-1],v=a[i],dt=v.at_ms-p.at_ms;if(p.at_ms<cut)continue;maxq=Math.max(maxq,p.queue,v.queue);if(dt>0&&dt<=3500){ms+=dt;drops+=Math.max(0,v.dropped-p.dropped);packets+=Math.max(0,v.packets-p.packets);}else gaps++;}var stale=!m.lastAt||now-m.lastAt>3500;return {observed_s:Math.round(ms/1000),dropped:drops,drops_per_min:ms?Math.round(drops*600000/ms)/10:null,packets_per_s:ms?Math.round(packets*10000/ms)/10:null,queue_max:maxq,gaps:gaps,stale:stale};}
 function exported(m,s,now){return {format:'BossQuickDock-diagnostics-1',generated_at:new Date(now).toISOString(),status:clean(s),recent:recent(m,now),stability:report(m,now),test_samples:m.test?m.test.samples.slice():[],recent_samples:m.history.slice(),note:'History collected in this browser tab; no PCM, tokens or pairing keys.'};}
 return {recent:recent,tick:tick,create:create,start:start,observe:observe,finish:finish,report:report,exported:exported,clean:clean};
})();
