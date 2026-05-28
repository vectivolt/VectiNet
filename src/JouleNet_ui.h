// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------
//
// JouleNet provisioning UI. Three tabs:
//   (1) Wi-Fi  — live network picker, hidden-SSID input, advanced reveal
//                (hostname, country, static IP), connect button.
//   (2) Setup  — custom-parameter form (text/password/number/toggle/
//                dropdown/color/textarea + headers + dividers).
//   (3) Status — live diagnostics (ssid, ip, rssi, mac, heap, uptime).
//
// All fetches go to /wifi/* (the route was renamed from /netwiz to drop the
// borrowed brand).
#pragma once
#include <Arduino.h>

namespace joule {
static const char NET_UI_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"/>
<meta name="theme-color" content="#0a0c12"/>
<title>__TITLE__</title>
<style>
:root{
  --bg:#0a0c12;--panel:rgba(255,255,255,.04);--panel-s:#141a2a;
  --ink:#e8ecf5;--ink2:#a5acc1;--muted:#6b7390;--line:rgba(255,255,255,.08);
  --brand:__BRAND__;--brand-2:#22d3ee;--ok:#3ddc97;--err:#ff6b81;--warn:#ffb347;
  --r:16px;--shadow:0 12px 40px rgba(0,0,0,.3);
  --grad:linear-gradient(135deg,var(--brand),var(--brand-2));
}
:root[data-theme="light"]{--bg:#f4f6fc;--panel:rgba(255,255,255,.75);--panel-s:#fff;--ink:#0f1730;--ink2:#3a4366;--line:rgba(15,23,48,.08);--shadow:0 10px 28px rgba(20,32,80,.08)}
@media(prefers-color-scheme:light){:root[data-theme="auto"]{--bg:#f4f6fc;--panel:rgba(255,255,255,.75);--panel-s:#fff;--ink:#0f1730;--ink2:#3a4366;--line:rgba(15,23,48,.08);--shadow:0 10px 28px rgba(20,32,80,.08)}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}html,body{margin:0;min-height:100%}
body{font:14.5px/1.5 -apple-system,BlinkMacSystemFont,"Inter","Segoe UI",Roboto,sans-serif;color:var(--ink);background:var(--bg);
  background-image:radial-gradient(1000px 500px at 10% -10%,color-mix(in srgb,var(--brand) 18%,transparent),transparent 60%),radial-gradient(900px 450px at 110% 10%,color-mix(in srgb,var(--brand-2) 14%,transparent),transparent 55%);
  background-attachment:fixed;min-height:100vh}
.wrap{max-width:560px;margin:0 auto;padding:18px}
header{display:flex;align-items:center;gap:12px;margin-bottom:14px}
.logo{width:38px;height:38px;border-radius:12px;background:var(--grad);display:grid;place-items:center;color:#fff;font-weight:800;font-size:16px;box-shadow:0 6px 20px color-mix(in srgb,var(--brand) 40%,transparent)}
h1{margin:0;font-size:18px;font-weight:700}
.sub{font-size:12px;color:var(--muted)}
.spacer{flex:1}
.iconbtn{width:36px;height:36px;border-radius:10px;border:1px solid var(--line);background:var(--panel);color:var(--ink);display:grid;place-items:center;cursor:pointer;transition:.15s}
.iconbtn:hover{border-color:var(--brand);color:var(--brand)}
.card{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);padding:16px;margin-bottom:12px;backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);box-shadow:var(--shadow)}
.tabs{display:flex;gap:6px;margin-bottom:12px;padding:4px;background:var(--panel);border-radius:14px;border:1px solid var(--line)}
.tab{flex:1;padding:9px 8px;border:0;background:transparent;color:var(--ink2);border-radius:10px;cursor:pointer;font:inherit;font-size:13px;font-weight:600;transition:.18s;min-height:38px}
.tab:hover{color:var(--ink)}
.tab.active{background:var(--grad);color:#fff;box-shadow:0 4px 12px color-mix(in srgb,var(--brand) 30%,transparent)}
.scan-head{display:flex;justify-content:space-between;align-items:center;margin-bottom:8px}
.scan-head .sub{font-weight:600;text-transform:uppercase;letter-spacing:.4px;font-size:11px;color:var(--muted)}
.ssid{display:flex;align-items:center;gap:10px;padding:11px 12px;border-radius:12px;cursor:pointer;border:1px solid transparent;transition:.15s;margin-bottom:4px}
.ssid:hover{background:color-mix(in srgb,var(--ink) 5%,transparent);border-color:var(--line)}
.ssid.sel{background:color-mix(in srgb,var(--brand) 12%,transparent);border-color:var(--brand)}
.bars{font-family:"SF Mono",ui-monospace,Menlo,monospace;color:var(--brand);font-size:14px;letter-spacing:1px}
.ssid .info{flex:1;min-width:0}
.ssid .name{font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.ssid .meta{font-size:10.5px;color:var(--muted);font-family:"SF Mono",ui-monospace,monospace}
.lock{font-size:12px;color:var(--muted)}
label.field{display:block;margin:10px 0}
label.field .lbl{display:block;font-size:11px;color:var(--muted);margin-bottom:4px;text-transform:uppercase;letter-spacing:.4px;font-weight:600}
input,select,textarea{width:100%;padding:11px 12px;border-radius:10px;border:1px solid var(--line);background:var(--panel);color:var(--ink);font:inherit;font-size:14px;outline:none;transition:.15s;min-height:42px}
textarea{min-height:80px;font-family:"SF Mono",ui-monospace,monospace;font-size:13px;resize:vertical}
input:focus,select:focus,textarea:focus{border-color:var(--brand);box-shadow:0 0 0 3px color-mix(in srgb,var(--brand) 22%,transparent)}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:11px 16px;border:0;border-radius:12px;background:var(--grad);color:#fff;font:inherit;font-weight:700;cursor:pointer;font-size:14px;width:100%;min-height:44px;box-shadow:0 4px 14px color-mix(in srgb,var(--brand) 28%,transparent);transition:.18s}
.btn:hover{transform:translateY(-1px);box-shadow:0 8px 20px color-mix(in srgb,var(--brand) 38%,transparent)}
.btn:active{transform:translateY(0)}
.btn:disabled{opacity:.5;cursor:not-allowed;transform:none}
.btn.ghost{background:var(--panel);color:var(--ink);border:1px solid var(--line);box-shadow:none}
.btn.danger{background:linear-gradient(135deg,#ff5470,#ff7e7e);box-shadow:0 4px 14px rgba(255,80,100,.3)}
.row{display:flex;gap:8px}.row>*{flex:1}
.adv{cursor:pointer;color:var(--brand);font-size:12px;margin-top:6px;display:inline-block;font-weight:600}
.hidden{display:none}
.toast{position:fixed;left:50%;bottom:20px;transform:translateX(-50%) translateY(20px);background:var(--panel-s);border:1px solid var(--line);padding:12px 18px;border-radius:12px;opacity:0;transition:.25s;pointer-events:none;z-index:9;box-shadow:var(--shadow);font-weight:600}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}.toast.ok{border-color:var(--ok)}.toast.err{border-color:var(--err)}
.kv{display:flex;justify-content:space-between;padding:9px 0;font-size:13px;border-bottom:1px dashed var(--line)}.kv:last-child{border:0}
.kv .k{color:var(--muted);text-transform:uppercase;font-size:11px;letter-spacing:.4px;font-weight:600;align-self:center}
.kv .v{font-family:"SF Mono",ui-monospace,Menlo,monospace;font-weight:600;font-size:13px}
.toggle{position:relative;width:50px;height:28px;background:var(--line);border-radius:99px;cursor:pointer;transition:.2s;border:1px solid var(--line)}
.toggle::after{content:"";position:absolute;left:2px;top:1px;width:22px;height:22px;background:#fff;border-radius:50%;transition:.22s cubic-bezier(.4,1.4,.6,1);box-shadow:0 2px 6px rgba(0,0,0,.25)}
.toggle.on{background:var(--grad);border-color:transparent}.toggle.on::after{left:24px}
footer{text-align:center;color:var(--muted);font-size:11px;margin:18px 0 30px}
.spinner{width:14px;height:14px;border:2px solid var(--line);border-top-color:var(--brand);border-radius:50%;animation:spin .9s linear infinite;display:inline-block;vertical-align:-2px}
@keyframes spin{to{transform:rotate(360deg)}}
.empty{padding:24px;text-align:center;color:var(--muted);font-size:13px}
</style></head><body>
<div class="wrap">
<header>
  <div class="logo">📶</div>
  <div><h1>__TITLE__</h1><div class="sub">JouleNet · pick a network &amp; apply settings</div></div>
  <div class="spacer"></div>
  <button class="iconbtn" id="themeBtn">◐</button>
</header>

<div class="card">
  <div class="tabs">
    <button class="tab active" data-pane="wifi">📶 Wi-Fi</button>
    <button class="tab" data-pane="params">⚙ Setup</button>
    <button class="tab" data-pane="status">📊 Status</button>
  </div>

  <div id="paneWifi">
    <div class="scan-head"><span class="sub">Available networks</span>
      <button class="btn ghost" id="rescan" style="width:auto;padding:6px 10px;min-height:30px;font-size:12px"><span id="rescanIco">⟳</span> Rescan</button></div>
    <div id="ssidList"><div class="empty"><span class="spinner"></span> scanning…</div></div>
    <label class="field"><span class="lbl">Password</span><input id="pwd" type="password" placeholder="leave empty for open networks"/></label>
    <label class="field"><span class="lbl">Or join hidden SSID</span><input id="hiddenSsid"/></label>
    <a class="adv" id="advToggle">▸ Advanced (static IP · hostname · country)</a>
    <div class="hidden" id="adv">
      <label class="field"><span class="lbl">Hostname (mDNS)</span><input id="host" placeholder="joule"/></label>
      <div class="row">
        <label class="field"><span class="lbl">Country</span><input id="cc" placeholder="01" maxlength="2"/></label>
        <label class="field"><span class="lbl">Static IP</span><input id="sip" placeholder="(DHCP)"/></label>
      </div>
      <div class="row">
        <label class="field"><span class="lbl">Gateway</span><input id="sgw"/></label>
        <label class="field"><span class="lbl">Netmask</span><input id="smask" placeholder="255.255.255.0"/></label>
      </div>
    </div>
    <button class="btn" id="connectBtn" style="margin-top:12px">⚡ Connect</button>
  </div>

  <div id="paneParams" class="hidden">
    <div id="paramHolder"><div class="empty"><span class="spinner"></span> loading…</div></div>
    <button class="btn" id="saveParams" style="margin-top:12px">💾 Save settings</button>
  </div>

  <div id="paneStatus" class="hidden">
    <div id="statusHolder"><div class="empty"><span class="spinner"></span> loading…</div></div>
    <div class="row" style="margin-top:12px">
      <button class="btn ghost" id="restartBtn">↻ Restart</button>
      <button class="btn danger" id="resetBtn">🗑 Erase &amp; reboot</button>
    </div>
  </div>
</div>
<footer>JouleNet · MIT · ESP32 / ESP8266 · Chinmoy Bhuyan</footer>
</div>
<div class="toast" id="toast"></div>

<script>
const $=s=>document.querySelector(s);
let selectedSsid=null,scanBusy=false,statusTimer=null;
function toast(m,k){const t=$("#toast");t.textContent=m;t.className="toast show "+(k||"");setTimeout(()=>t.className="toast",2400)}
function bars(rssi){const n=Math.min(4,Math.max(0,Math.round((rssi+90)/10)));return "▂▄▆█".substr(0,n).padEnd(4,"·")}

function applyTheme(){document.documentElement.setAttribute("data-theme",localStorage.getItem("joule-theme")||"auto")}
$("#themeBtn").onclick=()=>{const c=document.documentElement.getAttribute("data-theme")||"auto";const n=c==="auto"?"dark":(c==="dark"?"light":"auto");localStorage.setItem("joule-theme",n);applyTheme();toast("Theme: "+n)};
applyTheme();

document.querySelectorAll(".tab").forEach(b=>b.onclick=()=>{
  document.querySelectorAll(".tab").forEach(x=>x.classList.remove("active"));b.classList.add("active");
  ["wifi","params","status"].forEach(p=>$("#pane"+p[0].toUpperCase()+p.slice(1)).classList.toggle("hidden",p!==b.dataset.pane));
  if(b.dataset.pane==="params")loadParams();
  if(b.dataset.pane==="status"){loadStatus();if(!statusTimer)statusTimer=setInterval(loadStatus,3000)}else{clearInterval(statusTimer);statusTimer=null}
});

$("#advToggle").onclick=()=>{const a=$("#adv");a.classList.toggle("hidden");$("#advToggle").textContent=(a.classList.contains("hidden")?"▸":"▾")+" Advanced (static IP · hostname · country)"};

async function scan(){
  if(scanBusy)return;scanBusy=true;$("#rescanIco").innerHTML='<span class="spinner"></span>';
  try{
    const r=await fetch("/wifi/scan");const j=await r.json();
    if(!j.networks||j.networks.length===0){$("#ssidList").innerHTML='<div class="empty">no networks found — try again</div>'}
    else{$("#ssidList").innerHTML=j.networks.map(n=>`<div class="ssid" data-ssid="${(n.ssid||'').replace(/"/g,'&quot;')}"><span class="bars">${bars(n.rssi)}</span><div class="info"><div class="name">${n.ssid||"(hidden)"}</div><div class="meta">ch ${n.ch} · ${n.rssi}dBm · ${n.bssid}</div></div><span class="lock">${n.sec?"🔒":""}</span></div>`).join("");
      document.querySelectorAll(".ssid").forEach(el=>el.onclick=()=>{document.querySelectorAll(".ssid").forEach(x=>x.classList.remove("sel"));el.classList.add("sel");selectedSsid=el.dataset.ssid;$("#pwd").focus()})}
  }catch(e){toast("scan failed","err")}finally{scanBusy=false;$("#rescanIco").textContent="⟳"}
}
$("#rescan").onclick=scan;

$("#connectBtn").onclick=async()=>{
  const ssid=selectedSsid||$("#hiddenSsid").value.trim();
  if(!ssid){toast("pick a network","err");return}
  const body={ssid,password:$("#pwd").value,hidden:!selectedSsid&&!!$("#hiddenSsid").value,
    hostname:$("#host").value,countryCode:$("#cc").value,staticIp:$("#sip").value,gateway:$("#sgw").value,netmask:$("#smask").value};
  const btn=$("#connectBtn");btn.innerHTML='<span class="spinner"></span> Connecting…';btn.disabled=true;
  const r=await fetch("/wifi/connect",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
  if(r.ok)toast("Connecting — check Status tab","ok");else toast("Connect rejected","err");
  btn.innerHTML="⚡ Connect";btn.disabled=false;
};

async function loadParams(){
  const r=await fetch("/wifi/params"),j=await r.json();
  if(!j.params||!j.params.length){$("#paramHolder").innerHTML='<div class="empty">no custom parameters defined by this firmware</div>';return}
  $("#paramHolder").innerHTML=j.params.map(p=>{
    if(p.type==="header")return `<div style="font-weight:700;margin:14px 0 6px;color:var(--brand);font-size:12px;text-transform:uppercase;letter-spacing:.6px">${p.label}</div>`;
    if(p.type==="divider")return `<hr style="border:0;border-top:1px solid var(--line);margin:14px 0"/>`;
    const lbl=`<span class="lbl">${p.label}</span>`;
    if(p.type==="password")return `<label class="field">${lbl}<input data-k="${p.key}" type="password" value="${p.value||""}" placeholder="${p.hint||""}"/></label>`;
    if(p.type==="number")  return `<label class="field">${lbl}<input data-k="${p.key}" type="number" value="${p.value||""}" min="${p.min}" max="${p.max}"/></label>`;
    if(p.type==="toggle")  return `<label class="field" style="display:flex;justify-content:space-between;align-items:center">${lbl}<div data-k="${p.key}" class="toggle${p.value==="1"?" on":""}"></div></label>`;
    if(p.type==="dropdown"){const opts=(p.opts||"").split("|").map(o=>`<option value="${o}" ${o===p.value?"selected":""}>${o}</option>`).join("");return `<label class="field">${lbl}<select data-k="${p.key}">${opts}</select></label>`}
    if(p.type==="color")   return `<label class="field">${lbl}<input data-k="${p.key}" type="color" value="${p.value||"#3da9fc"}" style="width:80px;height:40px;padding:0"/></label>`;
    if(p.type==="textarea")return `<label class="field">${lbl}<textarea data-k="${p.key}" placeholder="${p.hint||""}">${p.value||""}</textarea></label>`;
    return `<label class="field">${lbl}<input data-k="${p.key}" value="${p.value||""}" placeholder="${p.hint||""}"/></label>`;
  }).join("");
  document.querySelectorAll(".toggle[data-k]").forEach(t=>t.onclick=()=>t.classList.toggle("on"));
}
$("#saveParams").onclick=async()=>{
  const body={};document.querySelectorAll("[data-k]").forEach(el=>{
    if(el.classList.contains("toggle"))body[el.dataset.k]=el.classList.contains("on")?"1":"0";
    else body[el.dataset.k]=el.value;
  });
  const r=await fetch("/wifi/params",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body)});
  toast(r.ok?"Saved":"Save failed",r.ok?"ok":"err");
};

const LABEL={state:"State",ssid:"SSID",ip:"IP",gateway:"Gateway",mask:"Netmask",dns:"DNS",bssid:"BSSID",channel:"Channel",rssi:"RSSI",hostname:"Hostname",mdns:"mDNS",mac:"MAC",heap:"Heap",uptime_s:"Uptime"};
const STATE=["idle","connecting","connected","portal","failed"];
async function loadStatus(){
  try{const r=await fetch("/wifi/status"),j=await r.json();
    if("state" in j)j.state=STATE[j.state]||j.state;
    if("uptime_s" in j)j.uptime_s=j.uptime_s+"s";
    if("heap" in j)j.heap=Math.round(j.heap/1024)+" KB";
    $("#statusHolder").innerHTML=Object.entries(j).map(([k,v])=>`<div class="kv"><span class="k">${LABEL[k]||k}</span><span class="v">${v}</span></div>`).join("");
  }catch(e){}
}
$("#resetBtn").onclick=async()=>{if(!confirm("Erase ALL Wi-Fi credentials and settings, then reboot?"))return;await fetch("/wifi/reset",{method:"POST"});toast("erased — rebooting","ok")};
$("#restartBtn").onclick=async()=>{await fetch("/wifi/restart",{method:"POST"});toast("restarting","ok")};

scan();
</script></body></html>)HTML";
} // namespace joule
