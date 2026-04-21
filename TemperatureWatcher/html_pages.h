#pragma once
#include <pgmspace.h>

// ===== Main live-data page =====
// Values are fetched dynamically via /api/data every 5 s — no full page reload.
static const char HTML_ROOT[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>BMP180 Station</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh;display:flex;flex-direction:column}
nav{background:#16213e;border-bottom:1px solid #2a2a4a;display:flex;padding:0 20px;flex-shrink:0}
nav a{color:#888;text-decoration:none;font-size:.9em;padding:14px 16px;border-bottom:2px solid transparent;display:inline-block}
nav a:hover{color:#eee}
nav a.on{color:#e94560;border-bottom-color:#e94560}
main{flex:1;display:flex;justify-content:center;align-items:center;padding:24px}
.card{background:#16213e;border-radius:16px;padding:32px 40px;box-shadow:0 8px 32px rgba(0,0,0,.4);min-width:320px}
h1{text-align:center;margin-bottom:24px;font-size:1.4em;color:#e94560}
.row{display:flex;justify-content:space-between;padding:12px 0;border-bottom:1px solid #2a2a4a}
.row:last-child{border-bottom:none}
.label{color:#888}.value{font-weight:bold;color:#e94560}
.footer{text-align:center;margin-top:16px;font-size:.8em;color:#555}
</style></head><body>
<nav>
  <a class="on" href="/">Live</a>
  <a href="/api/stats">History</a>
  <a href="/api/wifi-setup">WiFi</a>
</nav>
<main><div class="card">
<h1>BMP180 Weather Station</h1>
<div class="row"><span class="label">Temperature</span><span class="value" id="t">…</span></div>
<div class="row"><span class="label">Pressure</span><span class="value" id="p1">…</span></div>
<div class="row"><span class="label">Pressure</span><span class="value" id="p2">…</span></div>
<div class="row"><span class="label">Altitude</span><span class="value" id="al">…</span></div>
<div class="footer"><span id="ts"></span></div>
</div></main>
<script>
function upd(){
  fetch('/api/data').then(r=>r.json()).then(d=>{
    document.getElementById('t').textContent=d.temperature_c+' \u00b0C';
    document.getElementById('p1').textContent=d.pressure_hpa+' hPa';
    document.getElementById('p2').textContent=d.pressure_mmhg+' mmHg';
    document.getElementById('al').textContent=d.altitude_m+' m';
    document.getElementById('ts').textContent=new Date().toLocaleTimeString();
  });
}
upd();setInterval(upd,5000);
</script>
</body></html>)rawliteral";

// ===== WiFi provisioning / setup page =====
// Shown both in AP captive-portal mode and via /api/wifi-setup in normal mode.
static const char HTML_PROVISION[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TempWatcher Setup</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh;display:flex;flex-direction:column}
nav{background:#16213e;border-bottom:1px solid #2a2a4a;display:flex;padding:0 20px;flex-shrink:0}
nav a{color:#888;text-decoration:none;font-size:.9em;padding:14px 16px;border-bottom:2px solid transparent;display:inline-block}
nav a:hover{color:#eee}
nav a.on{color:#e94560;border-bottom-color:#e94560}
main{flex:1;display:flex;justify-content:center;align-items:center;padding:24px}
.card{background:#16213e;border-radius:16px;padding:32px 40px;
box-shadow:0 8px 32px rgba(0,0,0,.4);min-width:320px;width:100%;max-width:420px}
h1{text-align:center;margin-bottom:8px;font-size:1.3em;color:#e94560}
.sub{text-align:center;color:#888;font-size:.85em;margin-bottom:20px}
label{display:block;color:#888;font-size:.85em;margin-bottom:4px;margin-top:14px}
input{width:100%;padding:10px 12px;background:#0f0f1e;border:1px solid #2a2a4a;
border-radius:8px;color:#eee;font-size:1em;outline:none}
input:focus{border-color:#e94560}
#networks{margin-top:16px;max-height:200px;overflow-y:auto;border:1px solid #2a2a4a;
border-radius:8px;background:#0f0f1e}
.net{display:flex;justify-content:space-between;align-items:center;
padding:10px 14px;cursor:pointer;border-bottom:1px solid #1a1a2e;font-size:.9em}
.net:last-child{border-bottom:none}
.net:hover{background:#16213e}
.net .name{flex:1}
.net .meta{color:#888;font-size:.8em;margin-left:8px}
.lock{color:#e94560;margin-left:6px;font-size:.85em}
button{margin-top:20px;width:100%;padding:12px;background:#e94560;border:none;
border-radius:8px;color:#fff;font-size:1em;cursor:pointer;font-weight:bold}
button:hover{background:#c73652}
#status{margin-top:12px;text-align:center;font-size:.85em;color:#888}
#scan-btn{margin-top:12px;width:100%;padding:8px;background:#2a2a4a;border:none;
border-radius:8px;color:#aaa;font-size:.85em;cursor:pointer}
#scan-btn:hover{background:#3a3a5a}
</style></head><body>
<nav><a href="/">Live</a><a href="/api/stats">History</a><a class="on" href="/api/wifi-setup">WiFi</a></nav>
<main><div class="card">
<h1>WiFi Setup</h1>
<div class="sub">Connect TempWatcher to your network</div>
<div id="networks"><div class="net" style="color:#555;cursor:default">Scanning...</div></div>
<button id="scan-btn" onclick="scan()">Scan again</button>
<label for="ssid-in">Network name (SSID)</label>
<input id="ssid-in" type="text" placeholder="Enter SSID" maxlength="32">
<label for="pw-in">Password</label>
<input id="pw-in" type="password" placeholder="Leave empty for open network" maxlength="64">
<button onclick="save()">Save &amp; Connect</button>
<div id="status"></div>
</div>
<script>
function renderNets(nets){
  if(!nets.length){document.getElementById('networks').innerHTML='<div class="net" style="color:#555;cursor:default">No networks found</div>';return;}
  document.getElementById('networks').innerHTML=nets.map(n=>{
    var lock=n.enc?'<span class="lock">&#128274;</span>':'';
    return '<div class="net" onclick="pick(\''+n.ssid.replace(/'/g,"\\'")+'\')">'
      +'<span class="name">'+escH(n.ssid)+lock+'</span>'
      +'<span class="meta">'+n.rssi+' dBm</span></div>';
  }).join('');
}
function scan(retry){
  if(!retry) document.getElementById('networks').innerHTML='<div class="net" style="color:#555;cursor:default">Scanning...</div>';
  fetch('/api/scan').then(r=>r.json()).then(nets=>{
    if(!nets.length&&!retry){setTimeout(()=>scan(true),2000);return;}
    renderNets(nets);
  }).catch(()=>{document.getElementById('networks').innerHTML='<div class="net" style="color:#555;cursor:default">Scan failed</div>';});
}
function escH(s){var d=document.createElement('div');d.appendChild(document.createTextNode(s));return d.innerHTML;}
function pick(s){document.getElementById('ssid-in').value=s;document.getElementById('pw-in').focus();}
function save(){
  var s=document.getElementById('ssid-in').value.trim();
  var p=document.getElementById('pw-in').value;
  if(!s){document.getElementById('status').textContent='Please enter an SSID.';return;}
  document.getElementById('status').textContent='Saving...';
  var fd=new FormData();fd.append('ssid',s);fd.append('password',p);
  fetch('/api/save',{method:'POST',body:fd}).then(r=>{
    if(r.ok){document.getElementById('status').textContent='Saved! Device is restarting...';}
    else{document.getElementById('status').textContent='Error saving credentials.';}
  }).catch(()=>{document.getElementById('status').textContent='Saved! Device is restarting...';});
}
scan();
</script>
</div></main></body></html>)rawliteral";
