/*
 * Smart Air Purifier System by PARVEEN (Final Professional Version)
 * * STABILITY FIX v4 (Webserver & WiFi Priority):
 * - Added critical delay(2) in loop to prevent WiFi stack starvation.
 * - Optimized PMS7003 reading to prevent buffer overflows.
 * - Reduced LED refresh rate slightly to prioritize Web Server responsiveness.
 * - Fixed bracing/syntax errors.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// ------------------- HARDWARE CONFIG -------------------
#define LED_PIN        13 // D7 (GPIO 13)
#define NUMPIXELS      16
#define HOSTNAME       "Parveen-Purifier"

// --- FAN PINS ---
#define FAN_ENA        15 // D8
#define FAN_IN1        16 // D0
#define FAN_IN2        0  // D3
const int PWM_LIMIT = 460; // Max PWM for analogWrite on ESP8266 is usually 1023, ensure this matches your fan spec

// --- SENSORS ---
#define DHTPIN         2  // D4
#define DHTTYPE        DHT22
#define TOUCH_PIN      A0 

// PMS7003 Config
SoftwareSerial pmsSerial(14, 12); // RX (D5), TX (D6)

// OLED Config
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Components
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);
DHT dht(DHTPIN, DHTTYPE);

// Server
ESP8266WebServer server(80);
DNSServer dnsServer;

// ------------------- EEPROM CONFIG -------------------
#define EEPROM_SIZE 512
#define SETTINGS_MARKER 0xA5

struct Settings {
  uint8_t marker;
  char ssid[32];
  char pass[64];
  uint32_t ip; // Unused but kept for structure
  uint32_t gw;
  uint32_t sn;
};

// ------------------- DATA & STATE -------------------
struct PMS_Data {
  uint16_t pm10_env = 0;
  uint16_t pm25_env = 0;
  uint16_t pm100_env = 0;
};
PMS_Data pmsData;

// Settings
uint8_t targetR = 0, targetG = 255, targetB = 0; 
float currentR_f = 0, currentG_f = 255, currentB_f = 0;
uint8_t targetBrightness = 128;
bool isBreathing = false; 

// Modes
int currentMode = 3; 
uint16_t rainbowHue = 0;
bool fanAutoMode = true; 
int fanSpeedPercent = 0; 
float temperature = 0.0;
float humidity = 0.0;

// Display State
int displayPage = 0; // 0=Env, 1=Pollution, 2=Network
unsigned long lastTouchTime = 0;

// System Timers
bool isAPMode = false;
unsigned long lastWifiCheck = 0;
const long wifiCheckInterval = 30000;
unsigned long lastFrame = 0;
unsigned long lastPMSRead = 0;
unsigned long lastDHTRead = 0;

// ------------------- HTML CONTENT -------------------
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>PARVEEN SMART AIR PURIFIER</title>
  <style>
    :root { --bg: linear-gradient(135deg, #0f2027, #203a43, #2c5364); --card: rgba(255, 255, 255, 0.1); --acc: #00d2ff; --txt: #fff; }
    body { font-family: sans-serif; background: var(--bg); color: var(--txt); margin: 0; padding: 20px; display: flex; flex-direction: column; align-items: center; }
    h1 { font-weight: 300; letter-spacing: 2px; text-align: center; font-size: 1.4rem; }
    .card { background: var(--card); padding: 1.5rem; border-radius: 20px; width: 100%; max-width: 400px; margin-bottom: 20px; border: 1px solid rgba(255,255,255,0.2); }
    .grid { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 10px; text-align: center; }
    .item { background: rgba(0,0,0,0.3); padding: 10px; border-radius: 10px; }
    .val { font-size: 1.2rem; font-weight: bold; display: block; }
    .lbl { font-size: 0.7rem; opacity: 0.7; }
    .btn-group { display: flex; gap: 5px; background: rgba(0,0,0,0.3); padding: 5px; border-radius: 10px; margin-bottom: 15px; }
    .btn { flex: 1; background: none; border: none; color: #aaa; padding: 10px; border-radius: 8px; cursor: pointer; font-weight: bold; }
    .btn.active { background: var(--acc); color: #000; }
    .toggle { width: 100%; padding: 12px; background: rgba(0,0,0,0.2); border: 1px solid #555; color: #fff; border-radius: 10px; margin-bottom: 10px; cursor: pointer; }
    .toggle.on { background: #00d2ff; color: #000; font-weight: bold; border-color: #00d2ff; }
    input[type=range] { width: 100%; margin: 10px 0; }
    .temp-row { display: flex; justify-content: space-around; background: rgba(255,255,255,0.05); padding: 10px; border-radius: 10px; margin-bottom: 15px; }
    
    .live-indicator {
      display: inline-block; width: 10px; height: 10px; border-radius: 50%; background: #555; margin-right: 5px;
    }
    .live-on { background: #00ff00; box-shadow: 0 0 5px #00ff00; animation: blink 1s infinite; }
    .live-off { background: #ff0000; }
    @keyframes blink { 50% { opacity: 0.5; } }
  </style>
</head>
<body>
  <h1>PARVEEN SMART<br>AIR PURIFIER</h1>
  
  <div class="card">
    <div style="text-align:right; font-size:0.8rem; margin-bottom:5px;">
      <span id="liveDot" class="live-indicator"></span> <span id="connStatus">Connecting...</span>
    </div>
    
    <div id="status" style="text-align:center; padding:8px; border-radius:8px; background:#333; margin-bottom:15px; font-weight:bold;">Loading...</div>
    
    <div class="temp-row">
       <div><span id="temp" style="font-size:1.4rem; font-weight:bold;">--</span><br><span class="lbl">TEMP (°C)</span></div>
       <div><span id="hum" style="font-size:1.4rem; font-weight:bold;">--</span><br><span class="lbl">HUMIDITY (%)</span></div>
    </div>
    <div class="grid">
      <div class="item"><span class="val" id="p1">--</span><span class="lbl">PM 1.0</span></div>
      <div class="item"><span class="val" id="p2">--</span><span class="lbl">PM 2.5</span></div>
      <div class="item"><span class="val" id="p10">--</span><span class="lbl">PM 10</span></div>
    </div>
  </div>

  <div class="card">
    <h3>FAN CONTROL: <span id="fPct" style="color:var(--acc)">0%</span></h3>
    <button id="fAuto" class="toggle" onclick="tFan()">Auto Speed</button>
    <input type="range" id="fSlide" min="0" max="100" value="0" onchange="sFan()">
  </div>

  <div class="card">
    <h3>LED LIGHTING</h3>
    <div class="btn-group">
      <button class="btn" onclick="sMode(3)">AUTO</button>
      <button class="btn" onclick="sMode(0)">MANUAL</button>
      <button class="btn" onclick="sMode(1)">RAINBOW</button>
    </div>
    <button id="brth" class="toggle" onclick="tBr()">Breathing Effect</button>
    <div id="man" style="display:none; margin-bottom:10px;">
      <label>Color</label> <input type="color" id="col" value="#00d2ff" onchange="sParam()" style="width:100%; height:40px; border:none;">
    </div>
    <label>Brightness</label><input type="range" id="br" min="0" max="255" onchange="sParam()">
  </div>

<script>
let d={};

function u(j){
  d=j;
  document.getElementById('liveDot').className = 'live-indicator live-on';
  document.getElementById('connStatus').innerText = 'Live';

  document.getElementById('p1').innerText=j.pm1;
  document.getElementById('p2').innerText=j.pm25;
  document.getElementById('p10').innerText=j.pm10;
  document.getElementById('temp').innerText=j.t;
  document.getElementById('hum').innerText=j.h;
  
  let s=document.getElementById('status');
  if(j.pm25<=12){s.innerText="AIR: GOOD";s.style.background="#00AA00";}
  else if(j.pm25<=35){s.innerText="AIR: MODERATE";s.style.background="#CC8400";}
  else{s.innerText="AIR: HAZARD";s.style.background="#CC0000";}

  document.querySelectorAll('.btn').forEach((b,i)=>{
    b.classList.remove('active');
    if((i==0 && j.m==3)||(i==1 && j.m==0)||(i==2 && j.m==1)) b.classList.add('active');
  });

  let fa=document.getElementById('fAuto');
  let fs=document.getElementById('fSlide');
  if(j.fa){ fa.classList.add('on'); fa.innerText="Auto Speed: ON"; fs.disabled=true; }
  else{ fa.classList.remove('on'); fa.innerText="Auto Speed: OFF"; fs.disabled=false; }
  document.getElementById('fPct').innerText=j.fs+"%";
  if(document.activeElement!=fs) fs.value=j.fs;

  let br=document.getElementById('brth');
  if(j.bm){ br.classList.add('on'); br.innerText="Breathing: ON"; }
  else{ br.classList.remove('on'); br.innerText="Breathing: OFF"; }

  document.getElementById('man').style.display = (j.m==0)?'block':'none';
  if(document.activeElement!=document.getElementById('br')) document.getElementById('br').value=j.br;
}

function fetchLoop() {
  fetch('/get')
    .then(response => {
       if(!response.ok) throw new Error("Net");
       return response.json();
    })
    .then(data => u(data))
    .catch(e => {
       document.getElementById('liveDot').className = 'live-indicator live-off';
       document.getElementById('connStatus').innerText = 'Disconnected';
    });
}

function sMode(m){ fetch('/set?m='+m).then(sParam); }
function tFan(){ fetch('/set?fa='+(d.fa?0:1)).then(sParam); }
function tBr(){ fetch('/set?bm='+(d.bm?0:1)).then(sParam); }
function sFan(){ fetch('/set?fa=0&fs='+document.getElementById('fSlide').value); }
function sParam(){
  let c=document.getElementById('col').value;
  let r=parseInt(c.slice(1,3),16), g=parseInt(c.slice(3,5),16), b=parseInt(c.slice(5,7),16);
  fetch(`/set?r=${r}&g=${g}&b=${b}&br=${document.getElementById('br').value}`);
}

setInterval(fetchLoop, 2000); // Increased to 2s to reduce network load
</script></body></html>
)rawliteral";

// Config Page HTML
const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>Parveen Setup</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; background: #1a1a1a; color: white; display: flex; justify-content: center; align-items: center; min-height: 100vh; margin:0; }
  form { background: #333; padding: 30px; border-radius: 10px; box-shadow: 0 4px 15px rgba(0,0,0,0.5); text-align: center; width: 300px; }
  label { display: block; text-align: left; margin-top: 10px; color: #ccc; font-size: 0.9em; }
  input { width: 100%; padding: 12px; margin: 5px 0; border: none; border-radius: 4px; box-sizing: border-box; background: #444; color: white; font-size: 16px; }
  input:focus { outline: 2px solid #00d2ff; }
  button { width: 100%; padding: 12px; margin-top: 20px; background: #00d2ff; border: none; color: black; font-weight: bold; border-radius: 4px; cursor: pointer; }
</style>
</head>
<body>
  <form action="/save" method="POST">
    <h2 style="color:#00d2ff">Connect Parveen</h2>
    
    <label>WiFi SSID (Name)</label>
    <input type="text" name="ssid" placeholder="Enter WiFi Name" required>
    
    <label>WiFi Password (Visible)</label>
    <input type="text" name="pass" placeholder="Enter WiFi Password">
    
    <button type="submit">Save & Reboot</button>
  </form>
</body>
</html>
)rawliteral";

// ------------------- LOGIC -------------------

void setupAP() {
  isAPMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("PARVEEN AIR PURIFIER");
  dnsServer.start(53, "*", WiFi.softAPIP());
  
  server.on("/", [](){ server.send(200, "text/html", config_html); });
  
  server.on("/save", [](){
    String s = server.arg("ssid"); String p = server.arg("pass");
    
    Settings cfg;
    cfg.marker = SETTINGS_MARKER;
    s.toCharArray(cfg.ssid, 32);
    p.toCharArray(cfg.pass, 64);
    
    cfg.ip = 0; 
    
    EEPROM.put(0, cfg); EEPROM.commit();
    server.send(200, "text/html", "<h1>Saved!</h1><p>Rebooting...</p>");
    delay(1000); ESP.restart(); 
  });
  
  server.onNotFound([](){ server.send(200, "text/html", config_html); });
  server.begin();
  
  display.clearDisplay(); display.setTextSize(1); display.setCursor(0,0);
  display.println(F("SETUP REQUIRED")); display.println(F("Connect WiFi to:"));
  display.setTextSize(2); display.println(F("PARVEEN")); display.println(F("AIR PURIFIER")); display.display();
  currentMode=2; targetB=255; 
}

void factoryReset() {
  display.clearDisplay(); display.setCursor(0,20);
  display.setTextSize(2); display.println(F("RESETTING...")); display.display();
  
  Settings cfg;
  cfg.marker = 0; 
  EEPROM.put(0, cfg);
  EEPROM.commit();
  
  pixels.fill(pixels.Color(255,0,0)); pixels.show();
  delay(1000);
  ESP.restart();
}

void setup() {
  EEPROM.begin(EEPROM_SIZE);
  Serial.begin(115200);
  pmsSerial.begin(9600);
  dht.begin();
  
  pixels.begin(); pixels.show();
  pinMode(FAN_ENA, OUTPUT); pinMode(FAN_IN1, OUTPUT); pinMode(FAN_IN2, OUTPUT);
  digitalWrite(FAN_IN1, HIGH); digitalWrite(FAN_IN2, LOW); analogWrite(FAN_ENA, 0);

  if (analogRead(TOUCH_PIN) > 500) {
     delay(100);
     if (analogRead(TOUCH_PIN) > 500) { 
        if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {};
        factoryReset();
     }
  }

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println(F("OLED Fail"));
  display.clearDisplay(); display.setTextColor(SSD1306_WHITE);

  Settings cfg; EEPROM.get(0, cfg);
  
  WiFi.mode(WIFI_STA); 
  WiFi.hostname(HOSTNAME);
  WiFi.persistent(true); 
  WiFi.setAutoConnect(true);
  
  if (cfg.marker == SETTINGS_MARKER) {
    Serial.print("Connecting to EEPROM SSID: "); Serial.println(cfg.ssid);
    WiFi.begin(cfg.ssid, cfg.pass);
  } else {
    WiFi.begin(); 
  }
  
  int retries = 0;
  bool connected = false;
  
  display.clearDisplay(); display.setCursor(0,20);
  display.setTextSize(1); display.println(F("Connecting to:"));
  if(cfg.marker == SETTINGS_MARKER) { display.setTextSize(1); display.println(cfg.ssid); }
  display.display();

  while(retries < 50) { 
    if(WiFi.status() == WL_CONNECTED) { connected = true; break; }
    pixels.setPixelColor(retries % NUMPIXELS, pixels.Color(0,0,50)); pixels.show();
    delay(500); retries++;
  }
  
  if(connected) {
    pixels.fill(pixels.Color(0,150,0)); pixels.show(); delay(1000); 
    if(MDNS.begin(HOSTNAME)) MDNS.addService("http","tcp",80);
    
    server.on("/", [](){ server.send(200, "text/html", index_html); });
    server.on("/set", handleSet);
    server.on("/get", handleGet);
    server.enableCORS(true); 
    server.begin();
    
    displayPage = 2; 
    updateOLED(); 
    
    currentMode=3; 
  } else {
    setupAP();
  }
}

// Logic: Check frequently, but do not block
void readPMS() {
  if (pmsSerial.available() >= 32) {
    if (pmsSerial.peek() != 0x42) { 
      pmsSerial.read(); // Discard garbage
      return; 
    }
    
    uint8_t buffer[32];
    pmsSerial.readBytes(buffer, 32);
    
    if (buffer[0] == 0x42 && buffer[1] == 0x4D) {
      uint16_t sum = 0; for (uint8_t i = 0; i < 30; i++) sum += buffer[i];
      uint16_t check = (buffer[30] << 8) | buffer[31];
      if (sum == check) {
        pmsData.pm10_env = (buffer[10] << 8) | buffer[11];
        pmsData.pm25_env = (buffer[12] << 8) | buffer[13];
        pmsData.pm100_env = (buffer[14] << 8) | buffer[15];
        // Only update OLED if we are on the relevant page
        if(displayPage == 1 || displayPage == 0) updateOLED(); 
      }
    }
  }
}

void readDHT() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    temperature = t; humidity = h;
    // Don't update OLED here immediately to avoid I2C spam, let loop handle it or update if critical
  }
}

void checkTouch() {
  if (analogRead(TOUCH_PIN) > 500) {
    if (millis() - lastTouchTime > 300) { 
      displayPage++; if (displayPage > 2) displayPage = 0;
      lastTouchTime = millis();
      updateOLED();
    }
  }
}

void updateOLED() {
  display.clearDisplay();
  
  if (displayPage == 0) { 
    display.setTextSize(1); display.setCursor(10, 0); display.print(F("PARVEEN Environment"));
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
    display.setCursor(0, 22); display.setTextSize(2); display.print(F("PM2.5: ")); display.print(pmsData.pm25_env);
    display.setTextSize(1);
    display.setCursor(0, 48); display.print(F("Temp: ")); display.print(temperature, 1); display.print(F(" C"));
    display.setCursor(0, 57); display.print(F("Humi: ")); display.print(humidity, 1); display.print(F(" %"));
    
  } else if (displayPage == 1) { 
    display.setTextSize(1); display.setCursor(15, 0); display.print(F("PARVEEN Pollution"));
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
    display.setCursor(5, 20); display.print(F("PM 1.0 : ")); display.println(pmsData.pm10_env);
    display.setCursor(5, 35); display.print(F("PM 2.5 : ")); display.println(pmsData.pm25_env);
    display.setCursor(5, 50); display.print(F("PM 10  : ")); display.println(pmsData.pm100_env);
    
  } else if (displayPage == 2) { 
    display.setTextSize(1); display.setCursor(20, 0); display.print(F("PARVEEN Network"));
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
    display.setCursor(0, 25); display.print(F("IP: ")); display.print(WiFi.localIP());
    display.setCursor(0, 40); display.print(F("Fan Speed: ")); display.print(fanSpeedPercent); display.print("%");
    display.setCursor(0, 55); display.print(F("RSSI: ")); display.print(WiFi.RSSI());
  }
  display.display();
}

void handleSet() {
  if (server.hasArg("m")) currentMode = server.arg("m").toInt();
  if (server.hasArg("br")) targetBrightness = server.arg("br").toInt();
  if (server.hasArg("bm")) isBreathing = (server.arg("bm").toInt() == 1);
  if (server.hasArg("fa")) fanAutoMode = (server.arg("fa").toInt() == 1);
  if (server.hasArg("fs")) { int s = server.arg("fs").toInt(); if(!fanAutoMode) analogWrite(FAN_ENA, map(s,0,100,0,PWM_LIMIT)); fanSpeedPercent=s; }
  if (server.hasArg("r")) { targetR = server.arg("r").toInt(); targetG = server.arg("g").toInt(); targetB = server.arg("b").toInt(); }
  server.send(200, "text/plain", "OK");
}

void handleGet() {
  char buf[512]; 
  snprintf(buf, sizeof(buf), 
    "{\"pm1\":%d,\"pm25\":%d,\"pm10\":%d,\"t\":%.1f,\"h\":%.1f,\"m\":%d,\"br\":%d,\"bm\":%d,\"fa\":%d,\"fs\":%d}",
    pmsData.pm10_env, pmsData.pm25_env, pmsData.pm100_env, 
    temperature, humidity, 
    currentMode, targetBrightness, isBreathing?1:0, 
    fanAutoMode?1:0, fanSpeedPercent
  );
  server.send(200, "application/json", buf);
}

void loop() {
  unsigned long now = millis();
  
  if (isAPMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    // Simplified visual for AP mode
    if (now - lastFrame > 100) {
      lastFrame = now;
      pixels.fill(pixels.Color(0,0,50)); pixels.show();
    }
  } else {
    // 1. NETWORK & INPUT (Highest Priority)
    MDNS.update();
    server.handleClient();
    checkTouch();
    
    // 2. SENSOR READS (Non-blocking check)
    readPMS(); 
    
    // 3. PERIODIC SENSOR UPDATES (1 Second)
    if (now - lastPMSRead > 1000) {
      readDHT(); 
      lastPMSRead = now;
      if(displayPage == 0) updateOLED(); // Update temp on screen
      
      // Auto Logic
      if (currentMode == 3) {
        if (pmsData.pm25_env <= 12) { targetR=0; targetG=255; targetB=0; }
        else if (pmsData.pm25_env <= 35) { targetR=255; targetG=140; targetB=0; }
        else { targetR=255; targetG=0; targetB=0; }
      }
      if (fanAutoMode) {
        int tFan = 0;
        if (pmsData.pm25_env <= 12) tFan = 30;
        else if (pmsData.pm25_env <= 35) tFan = 60;
        else if (pmsData.pm25_env <= 55) tFan = 80;
        else tFan = 100;
        if (fanSpeedPercent != tFan) {
           analogWrite(FAN_ENA, map(tFan,0,100,0,PWM_LIMIT));
           fanSpeedPercent = tFan;
        }
      }
    }
    
    // 4. WIFI RECONNECT (30 Seconds)
    if (now - lastWifiCheck > wifiCheckInterval) {
      lastWifiCheck = now;
      if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    }

    // 5. LED ANIMATION (Slowed down to 40ms to save CPU for WiFi)
    if (now - lastFrame >= 40) {
      lastFrame = now;
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
  }
  
  // CRITICAL: Give WiFi stack time to process packets
  delay(2);
}