#pragma once

const char controlPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#eef3f1">
<link rel="icon" href="data:,">
<title>桌面灯板</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#eef3f1;color:#183a35;font-family:system-ui,sans-serif}
main{max-width:520px;margin:auto;padding:28px 20px 40px}h1{font-size:26px;margin:0 0 20px}
.card{background:#fff;border:1px solid #dce6e1;border-radius:20px;padding:20px;margin:14px 0}
#clock{font-size:40px;font-variant-numeric:tabular-nums;letter-spacing:2px;font-weight:650}
p{line-height:1.6;margin:8px 0;color:#546c65}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
button{font:inherit;background:#edf3f0;border:1px solid #d9e4dd;color:#23493e;border-radius:12px;padding:14px;cursor:pointer}
button[aria-pressed=true]{color:#fff;background:#236449;border-color:#236449}
button:disabled{opacity:.5}button.wide{width:100%;margin-top:12px}label{display:block;margin:10px 0}
input[type=range]{width:100%;height:36px;accent-color:#236449}
input:not([type=range]){width:100%;padding:13px;border:1px solid #cbd9d2;border-radius:10px;font:inherit}
textarea{width:100%;min-height:150px;resize:vertical;padding:13px;border:1px solid #cbd9d2;border-radius:10px;font:inherit;line-height:1.8}
.favorite{border-top:1px solid #edf1ee;padding:12px 0}.favorite p{color:#183a35;overflow-wrap:anywhere}
.favorite div{display:flex;gap:8px;flex-wrap:wrap}.favorite button{padding:9px 12px;font-size:14px}
#textNotice{color:#236449;min-height:24px}#textNow{overflow-wrap:anywhere}
summary{cursor:pointer;padding:4px 0;font-weight:600}#notice{min-height:24px;color:#236449}
#actions{display:flex;gap:10px;margin-top:12px}#actions[hidden]{display:none}
small{color:#6b7e76}.heading{display:flex;justify-content:space-between;align-items:center}
</style>
<main>
<h1>桌面灯板</h1>
<section class="card">
<div id="clock">--:--:--</div>
<p id="date">正在连接灯板</p>
<p id="weather">福州 · 天气暂不可用</p>
<button class="wide" id="sync">用手机时间校准</button>
</section>
<section class="card">
<div class="heading"><label for="brightness">整体亮度</label><strong id="level">--%</strong></div>
<input id="brightness" type="range" min="0" max="100" value="38" aria-label="整体亮度">
<small>0 为全暗，100 为当前 USB 功率设置的最高档。</small>
</section>
<section class="card">
<div class="grid" id="modes">
<button data-mode="calendar">日历时钟</button><button data-mode="pet">小鸭</button>
<button data-mode="waveform" data-requires-desktop disabled>彩色波形</button>
<button data-mode="spectrum" data-requires-desktop disabled>频谱柱</button>
<button data-mode="lamp">照明</button><button data-mode="newyear">新年动画</button>
<button data-mode="dino">小恐龙</button>
<button data-mode="phrases">中文语句</button>
</div>
<p id="modeHint"></p>
<p id="audioHint">电脑音频后台连接后，可直接在这里切换音乐律动。</p>
<div id="actions" hidden></div>
</section>
<section class="card" id="textCard">
<h2 style="font-size:19px;margin:0 0 10px">中文语句</h2>
<p id="textNow">每句停留 10 秒，按顺序循环播放。</p>
<form id="textForm">
<label for="phraseLines">轮播内容 · 一行一句</label>
<textarea id="phraseLines" maxlength="2000" spellcheck="false" placeholder="物来顺应&#10;未来不迎&#10;当时不杂&#10;既往不恋"></textarea>
<label for="phraseSeconds">每句显示时间（秒）</label>
<input id="phraseSeconds" type="number" min="3" max="120" step="1" value="10" required>
<p><small>最多 16 句，每句最多 32 个字。四个汉字整句显示，长句自动滚动。常用中文、英文和标点可用。</small></p>
<button class="wide" type="submit" id="saveText">保存并播放</button>
<button class="wide" type="button" id="favoriteText">收藏当前词句</button>
</form>
<p id="textNotice" role="status" aria-live="polite"></p>
<details><summary>常用词句 <span id="favoriteCount"></span></summary>
<p><small>保存在灯板里，换手机、断电后也能使用。</small></p>
<div id="favorites"></div>
</details>
</section>
<details class="card">
<summary>连接家里 Wi-Fi</summary>
<p id="wifi">未连接。联网后可自动校时、更新福州天气。</p>
<form id="network">
<label for="ssid">Wi-Fi 名称（2.4 GHz）</label><input id="ssid" name="ssid" maxlength="32" required autocomplete="off">
<label for="password">Wi-Fi 密码</label><input id="password" name="password" type="password" maxlength="63" autocomplete="off">
<button class="wide" type="submit">保存并连接</button>
</form>
</details>
<p id="notice" role="status" aria-live="polite"></p>
</main>
<script>
const byId=id=>document.getElementById(id);
let busy=false,dragging=false,brightnessTimer=null,latestState=null,previousActionMode=null;
let textConfiguration=null,textBusy=false;
async function api(path,values){
 const options=values?{method:'POST',body:new URLSearchParams(values)}:{cache:'no-store'};
 const response=await fetch('/api/'+path,options);
 const body=await response.json();
 if(!response.ok)throw new Error(body.error||'设置未成功');
 return body;
}
function feedback(message){byId('notice').textContent=message;}
function actionButton(label,action){
 const button=document.createElement('button');button.textContent=label;
 button.onclick=async()=>{try{await api('action',{action});}catch(error){feedback(error.message);}};
 byId('actions').appendChild(button);
}
function showState(state){
 latestState=state;
 byId('clock').textContent=state.timeValid?state.time.slice(11):'等待校时';
 byId('date').textContent=state.timeValid?state.time.slice(0,10):'校时前灯板显示小鸭';
 const weather=state.weather;
 byId('weather').textContent=weather.valid?
  '福州 · '+weather.condition+' '+weather.temperature+'°C'+(weather.ageSeconds>=1200?'（'+Math.floor(weather.ageSeconds/60)+'分钟前更新）':''):
  '福州 · 天气暂不可用';
 if(!dragging){byId('brightness').value=state.brightness;byId('level').textContent=state.brightness+'%';}
 document.querySelectorAll('[data-mode]').forEach(button=>button.setAttribute('aria-pressed',String(button.dataset.mode===state.mode)));
 document.querySelectorAll('[data-requires-desktop]').forEach(button=>{button.disabled=!state.desktopConnected;});
 const musicMode=state.mode==='waveform'||state.mode==='spectrum';
 byId('audioHint').textContent=!state.desktopConnected?
  '电脑音频后台未连接，请检查电脑和 USB 数据线连接。':
  musicMode&&state.desktopAudioStatus==='unavailable'?'电脑暂时无法取声，请检查电脑播放设备，程序会自动重试。':
  musicMode&&state.audioStreaming?'正在接收电脑声音，静音时显示跑马灯。':
  musicMode?'正在启动电脑取音…':'电脑音频后台已连接，可以直接切换彩色波形或频谱柱。';
 const names={calendar:'日历时钟',pet:'小鸭',lamp:'照明',newyear:'新年动画',dino:'小恐龙',waveform:'彩色波形',spectrum:'频谱柱',status:'电脑状态',phrases:'中文语句'};
 byId('textNow').textContent=state.mode==='phrases'?'正在显示第 '+(state.phraseIndex+1)+' 句 · '+state.phraseText:'选择中文语句即可按顺序循环播放。';
 byId('modeHint').textContent=(state.settingsPending?'正在保存 ':'已记住 ')+(names[state.mode]||state.mode)+(state.owner==='desktop'?' · 电脑控制中':'');
 byId('wifi').textContent=state.wifiConnected?'已连接，局域网控制地址 http://'+state.stationIp:
  state.wifiSaved?'正在尝试连接已保存的 Wi-Fi，可继续通过灯板热点操作。':'未连接。联网后可自动校时、更新福州天气。';
 if(previousActionMode!==state.mode){
  previousActionMode=state.mode;byId('actions').replaceChildren();
  if(state.mode==='pet'){actionButton('摸摸','pet');actionButton('玩水','play');}
  if(state.mode==='dino')actionButton('开始 / 跳跃','jump');
  byId('actions').hidden=byId('actions').children.length===0;
 }
}
async function refresh(){
 if(busy)return;
 busy=true;
 try{showState(await api('state'));}catch(error){feedback('暂时连不上灯板，请确认手机仍连接灯板热点或同一 Wi-Fi。');}
 finally{busy=false;}
}
async function syncTime(showMessage=true){
 const state=await api('time',{epoch:Math.floor(Date.now()/1000)});
 showState(state);if(showMessage)feedback('已校时，持续供电期间会独立走时。');
}
byId('sync').onclick=()=>syncTime().catch(error=>feedback(error.message));
byId('modes').onclick=async event=>{
 const button=event.target.closest('[data-mode]');if(!button)return;
 try{showState(await api('mode',{mode:button.dataset.mode}));feedback('模式已更新，会自动记住。');}
 catch(error){feedback(error.message);}
};
function editedLines(){return byId('phraseLines').value.split(/\r?\n/).map(line=>line.trim()).filter(Boolean);}
function textFeedback(message){byId('textNotice').textContent=message;}
function displayTextConfiguration(configuration,updateEditor=false){
 textConfiguration=configuration;
 if(updateEditor){byId('phraseLines').value=configuration.lines.join('\n');byId('phraseSeconds').value=configuration.seconds;}
 byId('favoriteCount').textContent='（'+configuration.favorites.length+'/32）';
 byId('favorites').replaceChildren();
 if(!configuration.favorites.length){const p=document.createElement('p');p.textContent='还没有收藏，在上面输入词句后点击收藏。';byId('favorites').appendChild(p);}
 configuration.favorites.forEach((line,index)=>{
  const row=document.createElement('div');row.className='favorite';
  const text=document.createElement('p');text.textContent=line;row.appendChild(text);
  const buttons=document.createElement('div');
  for(const [label,action] of [['替换轮播','replace'],['加入轮播','append'],['删除','delete']]){
   const button=document.createElement('button');button.type='button';button.textContent=label;
   button.onclick=async()=>{
    if(textBusy)return;
    if(action==='delete'){
     textBusy=true;
     try{const current=await api('text');displayTextConfiguration(await api('text/favorites',{lines:JSON.stringify(current.favorites.filter(value=>value!==line))}));textFeedback('已删除这条收藏。');}
     catch(error){textFeedback(error.message);}finally{textBusy=false;}
    }else{
     const lines=action==='replace'?[line]:editedLines();
     if(action==='append'&&!lines.includes(line))lines.push(line);
     if(lines.length>16){textFeedback('轮播最多 16 句，请先删掉一些。');return;}
     byId('phraseLines').value=lines.join('\n');textFeedback('已放入编辑框，点击“保存并播放”应用。');
    }
   };
   buttons.appendChild(button);
  }
  row.appendChild(buttons);byId('favorites').appendChild(row);
 });
}
byId('textForm').onsubmit=async event=>{
 event.preventDefault();if(textBusy)return;textBusy=true;byId('saveText').disabled=true;
 try{
  const configuration=await api('text',{config:JSON.stringify({lines:editedLines(),seconds:Number(byId('phraseSeconds').value)})});
  displayTextConfiguration(configuration,true);showState(await api('mode',{mode:'phrases'}));
  textFeedback('已保存，每句显示 '+configuration.seconds+' 秒，断电后也会记住。');
 }catch(error){textFeedback(error.message);}finally{textBusy=false;byId('saveText').disabled=false;}
};
byId('favoriteText').onclick=async()=>{
 if(textBusy)return;textBusy=true;
 try{
  const current=await api('text');const lines=editedLines();
  if(!lines.length)throw new Error('请先输入要收藏的词句。');
  const favorites=[...new Set([...current.favorites,...lines])];
  displayTextConfiguration(await api('text/favorites',{lines:JSON.stringify(favorites)}));
  textFeedback('已保存到常用词句，重复的内容会自动合并。');
 }catch(error){textFeedback(error.message);}finally{textBusy=false;}
};
const slider=byId('brightness');
slider.onpointerdown=()=>{dragging=true;};
slider.oninput=()=>{
 dragging=true;byId('level').textContent=slider.value+'%';
 clearTimeout(brightnessTimer);
 brightnessTimer=setTimeout(()=>saveBrightness(false),100);
};
async function saveBrightness(finished){
 const value=slider.value;
 try{await api('brightness',{value});if(finished)feedback('亮度已更新，会自动记住。');}
 catch(error){feedback(error.message);}
 finally{if(finished)dragging=false;}
}
slider.onchange=()=>{clearTimeout(brightnessTimer);saveBrightness(true);};
slider.onpointerup=()=>{dragging=false;};
byId('network').onsubmit=async event=>{
 event.preventDefault();
 try{
  await api('wifi',{ssid:byId('ssid').value,password:byId('password').value});
  byId('password').value='';feedback('Wi-Fi 已保存，正在连接。热点短暂重连后可刷新页面。');
 }catch(error){feedback(error.message);}
};
document.addEventListener('keydown',event=>{
 if(event.code==='Space'&&latestState?.mode==='dino'&&!['INPUT','TEXTAREA','BUTTON'].includes(event.target.tagName)){
  event.preventDefault();api('action',{action:'jump'}).catch(error=>feedback(error.message));
 }
});
refresh();
api('text').then(configuration=>displayTextConfiguration(configuration,true)).catch(error=>textFeedback(error.message));
syncTime(false).catch(()=>{});
setInterval(refresh,2000);
</script>
</html>)HTML";
