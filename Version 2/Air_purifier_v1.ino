/*
 * ESP32 SMART AIR PURIFIER - v2
 * PMS7003, Fan MOSFET, 128x64 OLED, DHT22, NeoPixels, Touch & Web Server
 *
 * Hardware Pins:
 * - PMS7003: RX=16, TX=17 (Hardware Serial 2)
 * - Fan PWM: 25
 * - OLED I2C: SDA=21, SCL=22
 * - DHT22: Pin 4
 * - NeoPixels: Pin 18 (16 LEDs)
 * - Touch Button: Pin 33
 *
 * v2 changes:
 * - All user settings (LED mode/colour/brightness/breathing, fan auto/speed,
 *   OLED page) are saved to EEPROM and restored on boot.
 * - Writes are debounced (3 s after last change) and skipped if unchanged,
 *   to protect flash from wear.
 * - Redesigned web dashboard; page background follows the LED colour.
 * - /get now also returns the live LED colour, IP and RSSI.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Adafruit_NeoPixel.h>

// ------------------- HARDWARE CONFIG -------------------
#define HOSTNAME       "ESP32-Purifier"

#define PMS_RX_PIN 16
#define PMS_TX_PIN 17
uint8_t pmsBuf[32];
int pmsIdx = 0;

#define FAN_PWM_PIN 25
const int pwmFreq = 5000;
const int pwmResolution = 8;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

#define LED_PIN 18
#define NUMPIXELS 16
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

#define TOUCH_PIN 33
bool lastTouchState = LOW;
unsigned long lastTouchTime = 0;

WebServer server(80);
DNSServer dnsServer;

// ------------------- EEPROM LAYOUT -------------------
#define EEPROM_SIZE      512
#define WIFI_ADDR        0
#define SETTINGS_MARKER  0xA5
#define USER_ADDR        256
#define USER_MARKER      0x5A

struct Settings {            // Wi-Fi credentials @ WIFI_ADDR
  uint8_t marker;
  char ssid[32];
  char pass[64];
  uint32_t ip, gw, sn;
};

struct UserSettings {        // User preferences @ USER_ADDR
  uint8_t marker;
  uint8_t mode;              // 0=Manual, 1=Rainbow, 3=Auto
  uint8_t r, g, b;           // manual colour
  uint8_t brightness;
  uint8_t breathing;
  uint8_t fanAuto;
  uint8_t fanSpeed;
  uint8_t page;
};

// ------------------- DATA & STATE -------------------
uint16_t pm10_env = 0, pm25_env = 0, pm100_env = 0;   // PM1.0, PM2.5, PM10
float temperature = 0.0, humidity = 0.0;

// LED
uint8_t manualR = 0, manualG = 210, manualB = 255;    // colour chosen by user
uint8_t targetR = 0, targetG = 255, targetB = 0;      // colour being shown
float currentR_f = 0, currentG_f = 0, currentB_f = 0;
uint8_t targetBrightness = 128;
bool isBreathing = false;
int currentMode = 3;
uint16_t rainbowHue = 0;

// Fan
bool fanAutoMode = true;
int fanSpeedPercent = 0;

// Persistence
bool settingsDirty = false;
unsigned long lastSettingChange = 0;
const unsigned long SAVE_DELAY = 3000;

// Timers
bool isAPMode = false;
unsigned long lastWifiCheck = 0;
const long wifiCheckInterval = 30000;
unsigned long lastFrame = 0;
unsigned long lastPMSRead = 0;
unsigned long lastDHTUpdate = 0;
int displayPage = 0;

// ------------------- DASHBOARD HTML -------------------
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Air Purifier</title>
<style>
@property --lr{syntax:'<number>';inherits:true;initial-value:0}
@property --lg{syntax:'<number>';inherits:true;initial-value:210}
@property --lb{syntax:'<number>';inherits:true;initial-value:255}
:root{--txt:#eef2f7;--muted:#8b96a8;--card:rgba(255,255,255,.06);--line:rgba(255,255,255,.12);--acc:#00d2ff}
*{box-sizing:border-box}
body{margin:0;font-family:Inter,system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;color:var(--txt);background:#0a0f1c;min-height:100vh;--lr:0;--lg:210;--lb:255;transition:--lr .9s,--lg .9s,--lb .9s}
body::before{content:'';position:fixed;inset:0;z-index:-1;background:
 radial-gradient(900px 500px at 15% -10%,rgb(var(--lr) var(--lg) var(--lb) / .55),transparent 60%),
 radial-gradient(700px 500px at 110% 110%,rgb(var(--lr) var(--lg) var(--lb) / .35),transparent 60%),#0a0f1c}
body.rainbow::before{background:linear-gradient(120deg,#ff0040,#ff8c00,#ffe600,#00e676,#00b0ff,#7c4dff,#ff0040);background-size:300% 300%;animation:rb 8s linear infinite;opacity:.45}
@keyframes rb{0%{background-position:0 50%}100%{background-position:300% 50%}}
.wrap{max-width:460px;margin:0 auto;padding:18px 16px 40px}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:14px}
h1{margin:0;font-size:1.1rem;letter-spacing:3px;font-weight:600}
h1 small{display:block;font-size:.65rem;letter-spacing:4px;color:var(--muted);font-weight:400}
.live{display:flex;align-items:center;gap:6px;font-size:.72rem;color:var(--muted)}
.dot{width:9px;height:9px;border-radius:50%;background:#555}
.on .dot{background:#2ecc71;box-shadow:0 0 8px #2ecc71;animation:pulse 1.4s infinite}
.off .dot{background:#e74c3c}
@keyframes pulse{50%{opacity:.4}}
.card{background:var(--card);backdrop-filter:blur(14px);-webkit-backdrop-filter:blur(14px);border:1px solid var(--line);border-radius:22px;padding:18px;margin-bottom:14px;box-shadow:0 10px 30px rgba(0,0,0,.35)}
.card h2{margin:0 0 12px;font-size:.78rem;letter-spacing:2px;color:var(--muted);font-weight:600;display:flex;justify-content:space-between;align-items:center}
.card h2 span{display:flex;align-items:center;gap:8px;font-size:.65rem}
.hero{display:flex;gap:16px;align-items:center}
.gauge{position:relative;width:140px;height:140px;flex-shrink:0}
.gauge svg{transform:rotate(-90deg)}
.gauge circle{fill:none;stroke-width:10;stroke-linecap:round}
.gauge .bg{stroke:rgba(255,255,255,.08)}
.gauge .fg{stroke:#2ecc71;stroke-dasharray:327;stroke-dashoffset:327;transition:stroke-dashoffset .8s ease,stroke .6s}
.gauge .c{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center}
.gauge .v{font-size:2rem;font-weight:700;line-height:1}
.gauge .u{font-size:.6rem;color:var(--muted);letter-spacing:1px;margin-top:4px}
.hs{flex:1}
.badge{display:inline-block;padding:6px 12px;border-radius:999px;font-size:.75rem;font-weight:700;letter-spacing:1px;background:#2ecc71;color:#06130b;transition:background .5s}
.kv{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid var(--line);font-size:.85rem}
.kv:last-child{border:0}.kv b{font-size:1rem}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:14px}
.tile{background:rgba(0,0,0,.28);border-radius:14px;padding:10px;text-align:center}
.tile .n{font-size:1.15rem;font-weight:700}.tile .l{font-size:.62rem;color:var(--muted);letter-spacing:1px}
.legend{display:flex;gap:6px;margin-top:12px}
.legend span{flex:1;font-size:.62rem;text-align:center;padding:5px 0;border-radius:8px;letter-spacing:.5px}
.row{display:flex;justify-content:space-between;align-items:center;margin:10px 0}
.sw{width:46px;height:26px;border-radius:999px;background:rgba(255,255,255,.15);border:0;position:relative;cursor:pointer;transition:background .3s;padding:0}
.sw::after{content:'';position:absolute;top:3px;left:3px;width:20px;height:20px;border-radius:50%;background:#fff;transition:transform .3s}
.sw.on{background:var(--acc)}
.sw.on::after{transform:translateX(20px)}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:6px;border-radius:3px;background:rgba(255,255,255,.15);outline:0;margin:8px 0}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:22px;height:22px;border-radius:50%;background:#fff;box-shadow:0 0 0 4px color-mix(in srgb,var(--acc) 45%,transparent);cursor:pointer}
input[type=range]:disabled{opacity:.35}
.seg{display:flex;background:rgba(0,0,0,.3);border-radius:12px;padding:4px;gap:4px}
.seg button{flex:1;border:0;background:none;color:var(--muted);padding:9px;border-radius:9px;font-weight:700;font-size:.75rem;letter-spacing:1px;cursor:pointer;transition:.25s}
.seg button.active{background:var(--acc);color:#000}
.fan{display:flex;align-items:center;gap:14px}
.fan svg{width:54px;height:54px;color:var(--acc);animation:spin 1s linear infinite;animation-play-state:paused}
@keyframes spin{to{transform:rotate(360deg)}}
.big{font-size:1.6rem;font-weight:700}
.ring{display:flex;justify-content:center;margin:6px 0 12px}
.ring circle{transition:fill .4s}
.sw-row{display:flex;gap:8px;margin:12px 0 10px}
.swatch{flex:1;height:30px;border-radius:9px;border:2px solid rgba(255,255,255,.15);cursor:pointer;transition:transform .15s}
.swatch:hover{transform:scale(1.1)}
.picker{position:relative;height:44px;border-radius:12px;overflow:hidden;border:1px solid var(--line)}
.picker input{position:absolute;inset:-10px;width:120%;height:140%;border:0;padding:0;cursor:pointer}
.picker span{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;pointer-events:none;font-weight:700;font-size:.78rem;letter-spacing:1px;text-shadow:0 1px 4px rgba(0,0,0,.7)}
#toast{position:fixed;left:50%;bottom:22px;transform:translate(-50%,20px);background:#111;color:#fff;padding:10px 18px;border-radius:999px;font-size:.8rem;opacity:0;transition:.3s;pointer-events:none;border:1px solid var(--line)}
#toast.show{opacity:1;transform:translate(-50%,0)}
footer{text-align:center;font-size:.65rem;color:var(--muted);margin-top:6px}
</style>
</head>
<body>
<div class="wrap">
<header>
 <h1>AIR PURIFIER<small>ESP32 SMART</small></h1>
 <div id="live" class="live"><span class="dot"></span><span id="ls">Connecting</span></div>
</header>

<div class="card">
 <div class="hero">
  <div class="gauge">
   <svg viewBox="0 0 120 120" width="140" height="140"><circle class="bg" cx="60" cy="60" r="52"/><circle id="arc" class="fg" cx="60" cy="60" r="52"/></svg>
   <div class="c"><div class="v" id="p2">--</div><div class="u">PM2.5 &micro;g/m&sup3;</div></div>
  </div>
  <div class="hs">
   <span id="badge" class="badge">&mdash;</span>
   <div class="kv" style="margin-top:10px"><span>&#127777; Temp</span><b><span id="temp">--</span>&deg;C</b></div>
   <div class="kv"><span>&#128167; Humidity</span><b><span id="hum">--</span>%</b></div>
  </div>
 </div>
 <div class="grid">
  <div class="tile"><div class="n" id="p1">--</div><div class="l">PM 1.0</div></div>
  <div class="tile"><div class="n" id="p25">--</div><div class="l">PM 2.5</div></div>
  <div class="tile"><div class="n" id="p10">--</div><div class="l">PM 10</div></div>
 </div>
 <div class="legend">
  <span style="background:rgba(46,204,113,.25)">GOOD 0&ndash;12</span>
  <span style="background:rgba(243,156,18,.25)">MODERATE 13&ndash;35</span>
  <span style="background:rgba(231,76,60,.25)">HAZARD &gt;35</span>
 </div>
</div>

<div class="card">
 <h2>FAN CONTROL <span>AUTO <button id="fAuto" class="sw" onclick="tFan()"></button></span></h2>
 <div class="fan">
  <svg id="fanIco" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="12" r="2.4"/><ellipse cx="12" cy="6" rx="3" ry="4.6"/><ellipse cx="12" cy="6" rx="3" ry="4.6" transform="rotate(120 12 12)"/><ellipse cx="12" cy="6" rx="3" ry="4.6" transform="rotate(240 12 12)"/></svg>
  <div style="flex:1"><div class="big"><span id="fPct">0</span>%</div><div id="fMode" style="font-size:.7rem;color:var(--muted)">&mdash;</div></div>
 </div>
 <input type="range" id="fSlide" min="0" max="100" value="0" oninput="fPrev(this.value)" onchange="sFan(this.value)">
</div>

<div class="card">
 <h2>LED LIGHTING <span>BREATHE <button id="brth" class="sw" onclick="tBr()"></button></span></h2>
 <div class="ring"><svg id="ringSvg" viewBox="0 0 100 100" width="110" height="110"></svg></div>
 <div class="seg">
  <button data-m="3" onclick="sMode(3)">AUTO</button>
  <button data-m="0" onclick="sMode(0)">MANUAL</button>
  <button data-m="1" onclick="sMode(1)">RAINBOW</button>
 </div>
 <div id="man" style="display:none">
  <div class="sw-row" id="swatches"></div>
  <div class="picker"><input type="color" id="col" value="#00d2ff" oninput="colPrev(this.value)" onchange="sCol(this.value)"><span>CUSTOM COLOUR</span></div>
 </div>
 <div class="row" style="margin-top:14px"><span style="font-size:.8rem">Brightness</span><b id="brV">--</b></div>
 <input type="range" id="br" min="0" max="255" oninput="brV.innerText=Math.round(this.value/255*100)+'%'" onchange="sBr(this.value)">
</div>

<footer id="net">&mdash;</footer>
</div>
<div id="toast"></div>

<script>
const $=i=>document.getElementById(i);
let d={},tm;
const PRESETS=['#ff3b30','#ff9500','#ffd60a','#34c759','#00d2ff','#5e5ce6','#ff2d55','#ffffff'];
const hex=(r,g,b)=>'#'+[r,g,b].map(v=>(+v).toString(16).padStart(2,'0')).join('');
const h2r=h=>[1,3,5].map(i=>parseInt(h.slice(i,i+2),16));

(function(){
 let s='';
 for(let i=0;i<16;i++){const a=i/16*2*Math.PI;s+=`<circle cx="${(50+40*Math.cos(a)).toFixed(1)}" cy="${(50+40*Math.sin(a)).toFixed(1)}" r="5" fill="#00d2ff"/>`;}
 $('ringSvg').innerHTML=s;
 $('swatches').innerHTML=PRESETS.map(c=>`<div class="swatch" style="background:${c}" onclick="sCol('${c}')"></div>`).join('');
})();

function toast(t,err){const e=$('toast');e.innerText=t;e.style.background=err?'#b3261e':'#111';e.classList.add('show');clearTimeout(tm);tm=setTimeout(()=>e.classList.remove('show'),1400);}
function api(q){return fetch('/set?'+q).then(r=>{if(!r.ok)throw 0;toast('Saved');}).catch(()=>toast('Could not reach purifier',1));}

function setTheme(r,g,b,rainbow){
 document.body.classList.toggle('rainbow',!!rainbow);
 const st=document.body.style;
 st.setProperty('--lr',r);st.setProperty('--lg',g);st.setProperty('--lb',b);
 st.setProperty('--acc',rainbow?'#ffffff':(r+g+b<90?'#00d2ff':hex(r,g,b)));
 const ring=$('ringSvg');
 ring.querySelectorAll('circle').forEach((c,i)=>{c.style.fill=rainbow?`hsl(${i/16*360},100%,55%)`:hex(r,g,b);});
 ring.style.filter=`drop-shadow(0 0 6px ${rainbow?'#fff':hex(r,g,b)})`;
}
function spin(v){const f=$('fanIco');if(v>0){f.style.animationDuration=(2.3-v/100*2)+'s';f.style.animationPlayState='running';}else f.style.animationPlayState='paused';}
function fPrev(v){$('fPct').innerText=v;spin(v);}

function render(j){
 d=j;
 $('live').className='live on';$('ls').innerText='Live';
 $('p1').innerText=j.pm1;$('p25').innerText=j.pm25;$('p2').innerText=j.pm25;$('p10').innerText=j.pm10;
 $('temp').innerText=j.t;$('hum').innerText=j.h;
 let col,txt;
 if(j.pm25<=12){col='#2ecc71';txt='GOOD';}else if(j.pm25<=35){col='#f39c12';txt='MODERATE';}else{col='#e74c3c';txt='HAZARDOUS';}
 const bd=$('badge');bd.innerText='AIR '+txt;bd.style.background=col;
 const arc=$('arc');arc.style.stroke=col;arc.style.strokeDashoffset=327-327*Math.min(j.pm25,150)/150;
 document.querySelectorAll('.seg button').forEach(x=>x.classList.toggle('active',+x.dataset.m===j.m));
 $('man').style.display=j.m==0?'block':'none';
 $('fAuto').classList.toggle('on',!!j.fa);
 $('fMode').innerText=j.fa?'Auto speed based on PM2.5':'Manual speed';
 const fs=$('fSlide');fs.disabled=!!j.fa;
 if(document.activeElement!==fs){fs.value=j.fs;$('fPct').innerText=j.fs;}
 spin(j.fs);
 $('brth').classList.toggle('on',!!j.bm);
 const br=$('br');if(document.activeElement!==br){br.value=j.br;$('brV').innerText=Math.round(j.br/255*100)+'%';}
 if(j.m==0&&document.activeElement!==$('col'))$('col').value=hex(j.r,j.g,j.b);
 setTheme(j.r,j.g,j.b,j.m==1);
 $('net').innerText=`${j.ip}  \u00b7  ${j.rssi} dBm`;
}

function poll(){
 const c=new AbortController();const t=setTimeout(()=>c.abort(),1800);
 fetch('/get',{signal:c.signal}).then(r=>r.json()).then(render)
  .catch(()=>{$('live').className='live off';$('ls').innerText='Offline';})
  .finally(()=>clearTimeout(t));
}
function sMode(m){d.m=m;if(m==0){[d.r,d.g,d.b]=h2r($('col').value);}render(d);api('m='+m).then(poll);}
function tFan(){api('fa='+(d.fa?0:1)).then(poll);}
function tBr(){api('bm='+(d.bm?0:1)).then(poll);}
function sFan(v){api('fa=0&fs='+v);}
function sBr(v){api('br='+v);}
function colPrev(h){const [r,g,b]=h2r(h);setTheme(r,g,b,false);}
function sCol(h){const [r,g,b]=h2r(h);$('col').value=h;d.m=0;d.r=r;d.g=g;d.b=b;render(d);api(`m=0&r=${r}&g=${g}&b=${b}`);}
poll();setInterval(poll,2000);
</script>
</body></html>
)rawliteral";

// ------------------- CONFIG PAGE HTML -------------------
const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>Purifier Setup</title>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
  body{font-family:system-ui,sans-serif;background:radial-gradient(800px 500px at 20% -10%,rgba(0,210,255,.35),transparent 60%),#0a0f1c;color:#fff;display:flex;justify-content:center;align-items:center;min-height:100vh;margin:0}
  form{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.12);backdrop-filter:blur(14px);padding:30px;border-radius:22px;width:320px;box-shadow:0 10px 30px rgba(0,0,0,.4)}
  h2{margin:0 0 4px;color:#00d2ff;letter-spacing:2px;font-size:1.1rem}
  p{margin:0 0 18px;color:#8b96a8;font-size:.8rem}
  label{display:block;margin-top:12px;color:#8b96a8;font-size:.75rem;letter-spacing:1px}
  input{width:100%;padding:12px;margin:6px 0;border:1px solid rgba(255,255,255,.15);border-radius:10px;box-sizing:border-box;background:rgba(0,0,0,.3);color:#fff;font-size:16px}
  input:focus{outline:2px solid #00d2ff}
  button{width:100%;padding:13px;margin-top:22px;background:#00d2ff;border:0;color:#000;font-weight:700;border-radius:10px;cursor:pointer;letter-spacing:1px}
</style>
</head>
<body>
  <form action="/save" method="POST">
    <h2>CONNECT PURIFIER</h2>
    <p>Enter your home Wi-Fi details</p>
    <label>WIFI NAME (SSID)</label>
    <input type="text" name="ssid" placeholder="Enter WiFi Name" required>
    <label>WIFI PASSWORD</label>
    <input type="text" name="pass" placeholder="Enter WiFi Password">
    <button type="submit">Save &amp; Reboot</button>
  </form>
</body>
</html>
)rawliteral";

// ------------------- PERSISTENCE -------------------
void applyFan(int pct) {
  pct = constrain(pct, 0, 100);
  ledcWrite(FAN_PWM_PIN, map(pct, 0, 100, 0, 255));
  fanSpeedPercent = pct;
}

void loadUserSettings() {
  UserSettings u;
  EEPROM.get(USER_ADDR, u);
  if (u.marker != USER_MARKER) {
    Serial.println("No saved user settings, using defaults");
    return;
  }
  currentMode      = (u.mode == 0 || u.mode == 1 || u.mode == 3) ? u.mode : 3;
  manualR = u.r; manualG = u.g; manualB = u.b;
  targetBrightness = u.brightness;
  isBreathing      = u.breathing;
  fanAutoMode      = u.fanAuto;
  fanSpeedPercent  = constrain(u.fanSpeed, 0, 100);
  displayPage      = (u.page <= 2) ? u.page : 0;
  if (currentMode == 0) { targetR = manualR; targetG = manualG; targetB = manualB; }
  Serial.printf("Restored: mode=%d rgb=%d,%d,%d br=%d breath=%d fanAuto=%d fan=%d page=%d\n",
                currentMode, manualR, manualG, manualB, targetBrightness, isBreathing,
                fanAutoMode, fanSpeedPercent, displayPage);
}

void markDirty() {
  settingsDirty = true;
  lastSettingChange = millis();
}

void saveUserSettingsIfNeeded() {
  if (!settingsDirty || millis() - lastSettingChange < SAVE_DELAY) return;
  settingsDirty = false;

  UserSettings u;
  u.marker     = USER_MARKER;
  u.mode       = currentMode;
  u.r = manualR; u.g = manualG; u.b = manualB;
  u.brightness = targetBrightness;
  u.breathing  = isBreathing ? 1 : 0;
  u.fanAuto    = fanAutoMode ? 1 : 0;
  u.fanSpeed   = fanSpeedPercent;
  u.page       = displayPage;

  UserSettings old;
  EEPROM.get(USER_ADDR, old);
  if (memcmp(&u, &old, sizeof(u)) == 0) return;   // nothing changed, spare the flash

  EEPROM.put(USER_ADDR, u);
  EEPROM.commit();
  Serial.println("User settings saved");
}

// ------------------- AP / SETUP MODE -------------------
void setupAP() {
  isAPMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32 AIR PURIFIER");
  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", [](){ server.send_P(200, "text/html", config_html); });
  server.on("/save", [](){
    String s = server.arg("ssid");
    String p = server.arg("pass");
    Settings cfg;
    cfg.marker = SETTINGS_MARKER;
    s.toCharArray(cfg.ssid, 32);
    p.toCharArray(cfg.pass, 64);
    cfg.ip = cfg.gw = cfg.sn = 0;
    EEPROM.put(WIFI_ADDR, cfg);
    EEPROM.commit();
    server.send(200, "text/html", "<h1>Saved!</h1><p>Rebooting...</p>");
    delay(1000);
    ESP.restart();
  });
  server.onNotFound([](){ server.send_P(200, "text/html", config_html); });
  server.begin();

  display.clearDisplay(); display.setTextSize(1); display.setCursor(0,0);
  display.println(F("SETUP REQUIRED")); display.println(F("Connect WiFi to:"));
  display.setTextSize(2); display.println(F("ESP32 AIR")); display.println(F("PURIFIER")); display.display();
}

void setup() {
  EEPROM.begin(EEPROM_SIZE);
  Serial.begin(115200);

  loadUserSettings();                 // restore before any hardware is driven

  Serial2.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);
  dht.begin();
  pixels.begin(); pixels.show();
  pinMode(TOUCH_PIN, INPUT);

  ledcAttach(FAN_PWM_PIN, pwmFreq, pwmResolution);
  applyFan(fanSpeedPercent);          // fan starts at last known speed immediately

  Wire.begin(21, 22);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println(F("OLED Fail"));
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE);

  Settings cfg; EEPROM.get(WIFI_ADDR, cfg);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  if (cfg.marker == SETTINGS_MARKER) {
    Serial.print("Connecting to EEPROM SSID: "); Serial.println(cfg.ssid);
    WiFi.begin(cfg.ssid, cfg.pass);
  } else {
    WiFi.begin();
  }

  display.clearDisplay(); display.setCursor(0,20); display.setTextSize(1);
  display.println(F("Connecting to:"));
  if(cfg.marker == SETTINGS_MARKER) display.println(cfg.ssid);
  display.display();

  int retries = 0; bool connected = false;
  while(retries < 30) {
    if(WiFi.status() == WL_CONNECTED) { connected = true; break; }
    pixels.setPixelColor(retries % NUMPIXELS, pixels.Color(0,0,50)); pixels.show();
    delay(500); retries++;
  }

  if(connected) {
    pixels.fill(pixels.Color(0,150,0)); pixels.show(); delay(1000);
    if(MDNS.begin(HOSTNAME)) MDNS.addService("http","tcp",80);

    server.on("/", [](){ server.send_P(200, "text/html", index_html); });
    server.on("/set", handleSet);
    server.on("/get", handleGet);
    server.enableCORS(true);
    server.begin();
    updateOLED();
  } else {
    setupAP();
  }
}

// ------------------- WEB API -------------------
void handleSet() {
  if (server.hasArg("m")) {
    int m = server.arg("m").toInt();
    if (m == 0 || m == 1 || m == 3) currentMode = m;
  }
  if (server.hasArg("br")) targetBrightness = constrain(server.arg("br").toInt(), 0, 255);
  if (server.hasArg("bm")) isBreathing = (server.arg("bm").toInt() == 1);
  if (server.hasArg("fa")) fanAutoMode = (server.arg("fa").toInt() == 1);
  if (server.hasArg("fs")) {
    int s = constrain(server.arg("fs").toInt(), 0, 100);
    if (!fanAutoMode) applyFan(s); else fanSpeedPercent = s;
  }
  if (server.hasArg("r") && server.hasArg("g") && server.hasArg("b")) {
    manualR = constrain(server.arg("r").toInt(), 0, 255);
    manualG = constrain(server.arg("g").toInt(), 0, 255);
    manualB = constrain(server.arg("b").toInt(), 0, 255);
  }
  if (currentMode == 0) { targetR = manualR; targetG = manualG; targetB = manualB; }
  markDirty();
  server.send(200, "text/plain", "OK");
}

void handleGet() {
  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"pm1\":%d,\"pm25\":%d,\"pm10\":%d,\"t\":%.1f,\"h\":%.1f,"
    "\"m\":%d,\"br\":%d,\"bm\":%d,\"fa\":%d,\"fs\":%d,"
    "\"r\":%d,\"g\":%d,\"b\":%d,\"ip\":\"%s\",\"rssi\":%d}",
    pm10_env, pm25_env, pm100_env, temperature, humidity,
    currentMode, targetBrightness, isBreathing?1:0, fanAutoMode?1:0, fanSpeedPercent,
    targetR, targetG, targetB, WiFi.localIP().toString().c_str(), WiFi.RSSI());
  server.send(200, "application/json", buf);
}

// ------------------- SENSORS -------------------
void readPMS() {
  while (Serial2.available()) {
    uint8_t c = Serial2.read();
    if (pmsIdx == 0 && c != 0x42) { pmsIdx = 0; continue; }
    if (pmsIdx == 1 && c != 0x4D) { pmsIdx = 0; continue; }
    pmsBuf[pmsIdx++] = c;
    if (pmsIdx >= 32) {
      uint16_t sum = 0;
      for (uint8_t i = 0; i < 30; i++) sum += pmsBuf[i];
      if (sum == ((pmsBuf[30] << 8) | pmsBuf[31])) {
        pm10_env  = (pmsBuf[10] << 8) | pmsBuf[11];
        pm25_env  = (pmsBuf[12] << 8) | pmsBuf[13];
        pm100_env = (pmsBuf[14] << 8) | pmsBuf[15];
      }
      pmsIdx = 0;
    }
  }
}

void readDHT() {
  if (millis() - lastDHTUpdate >= 2000) {
    lastDHTUpdate = millis();
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    if (!isnan(h) && !isnan(t)) { humidity = h; temperature = t; }
  }
}

// ------------------- OLED -------------------
void updateOLED() {
  display.clearDisplay();
  if (displayPage == 0) {
    display.setTextSize(1); display.setCursor(15, 0); display.print(F("Environment Data"));
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
    display.setCursor(0, 15); display.setTextSize(2); display.print(F("PM2.5:")); display.print(pm25_env);
    display.setTextSize(1);
    display.setCursor(0, 35); display.print(F("Temp: ")); display.print(temperature, 1); display.print(F(" C"));
    display.setCursor(0, 45); display.print(F("Humi: ")); display.print(humidity, 1); display.print(F(" %"));
    display.setCursor(0, 55); display.print(F("Fan : ")); display.print(fanSpeedPercent); display.print(F(" %"));
  } else if (displayPage == 1) {
    display.setTextSize(1); display.setCursor(15, 0); display.print(F("Pollutant Levels"));
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
    display.setCursor(5, 20); display.print(F("PM 1.0 : ")); display.println(pm10_env);
    display.setCursor(5, 35); display.print(F("PM 2.5 : ")); display.println(pm25_env);
    display.setCursor(5, 50); display.print(F("PM 10  : ")); display.println(pm100_env);
  } else if (displayPage == 2) {
    display.setTextSize(1); display.setCursor(25, 0); display.print(F("Network Info"));
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
    display.setCursor(0, 25); display.print(F("IP: ")); display.print(WiFi.localIP());
    display.setCursor(0, 40); display.print(F("Fan: ")); display.print(fanSpeedPercent); display.print(F(" %"));
    display.setCursor(0, 55); display.print(F("RSSI: ")); display.print(WiFi.RSSI());
  }
  display.display();
}

// ------------------- MAIN LOOP -------------------
void loop() {
  unsigned long now = millis();

  if (isAPMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    if (now - lastFrame > 100) {
      lastFrame = now;
      pixels.setBrightness(255);
      pixels.fill(pixels.Color(0,0,50)); pixels.show();
    }
  } else {
    server.handleClient();
    readPMS();
    readDHT();

    // Touch button: rotate OLED page
    bool currentTouchState = digitalRead(TOUCH_PIN);
    if (currentTouchState == HIGH && lastTouchState == LOW) {
      if (now - lastTouchTime > 300) {
        displayPage = (displayPage + 1) % 3;
        updateOLED();
        markDirty();
        lastTouchTime = now;
      }
    }
    lastTouchState = currentTouchState;

    // 1 Hz logic
    if (now - lastPMSRead > 1000) {
      lastPMSRead = now;
      if (currentMode == 3) {
        if (pm25_env <= 12)      { targetR=0;   targetG=255; targetB=0; }
        else if (pm25_env <= 35) { targetR=255; targetG=140; targetB=0; }
        else                     { targetR=255; targetG=0;   targetB=0; }
      }
      if (fanAutoMode) {
        int tFan;
        if (pm25_env <= 12)      tFan = 30;
        else if (pm25_env <= 35) tFan = 60;
        else if (pm25_env <= 55) tFan = 80;
        else                     tFan = 100;
        if (fanSpeedPercent != tFan) applyFan(tFan);
      }
      updateOLED();
    }

    // Wi-Fi watchdog
    if (now - lastWifiCheck > wifiCheckInterval) {
      lastWifiCheck = now;
      if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    }

    // LED animation @ 25 fps
    if (now - lastFrame >= 40) {
      lastFrame = now;
      if (currentMode == 0) { targetR = manualR; targetG = manualG; targetB = manualB; }
      float ease = 0.1;
      currentR_f += (targetR - currentR_f) * ease;
      currentG_f += (targetG - currentG_f) * ease;
      currentB_f += (targetB - currentB_f) * ease;

      uint8_t br = targetBrightness;
      if (isBreathing) {
        float breath = (exp(sin(now / 1500.0 * 3.14)) - 0.368) * 108.0;
        br = map(breath, 0, 255, 10, targetBrightness);
      }
      pixels.setBrightness(br);
      if (currentMode == 1) { rainbowHue += 100; pixels.rainbow(rainbowHue); }
      else pixels.fill(pixels.Color((uint8_t)currentR_f, (uint8_t)currentG_f, (uint8_t)currentB_f));
      pixels.show();
    }

    saveUserSettingsIfNeeded();
  }
  delay(2);
}
