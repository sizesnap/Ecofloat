/*
 * EcoFloat — Base Station (FINAL + on-device CSV log + KML mission nav)
 * Board: Heltec WiFi LoRa 32 V3
 * LoRa params mirror boat v3: SF9, CR 4/7, sync 0x12, preamble 8
 * Dashboard: connect to WiFi "EcoFloat-Base" (pw 12345678) -> http://192.168.4.1
 *
 * FEATURES:
 *   - Receives LoRa telemetry from the boat (position + DO/temp sensors).
 *   - Circular buffer logs every packet on-device (LOG_CAPACITY readings).
 *   - /csv streams the FULL log as a CSV download (survives page refresh /
 *     phone disconnect; lost only on base-station power cycle).
 *   - Live satellite map (Leaflet + Esri World Imagery) with the boat's
 *     position and a DO-colored breadcrumb trail. Satellite tiles need
 *     internet on YOUR PHONE (cellular works while joined to EcoFloat-Base).
 *     With no signal the map shows a notice; the offline SVG trail still works.
 *   - Leaflet + toGeoJSON load ASYNC so no missing-internet stall.
 *   - KML MISSION UPLOAD. Upload a waypoint KML (drawn in Google Earth)
 *     from the dashboard. It's stored on LittleFS (survives reboots) and
 *     parsed in the browser with toGeoJSON. Waypoints draw on the satellite
 *     map as a numbered, dashed route.
 *   - SEMI-ASSISTED WAYPOINT NAV. A nav panel shows the compass bearing
 *     to the active waypoint, distance, course-over-ground (derived from
 *     consecutive GPS fixes — the boat has no magnetometer), and a relative
 *     turn arrow ("turn left 20 deg"). Auto-advances to the next waypoint
 *     within ARRIVE_M metres; tap a chip to jump the target manually.
 */

#include <RadioLib.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <LittleFS.h>   // persistent storage for the uploaded KML mission

// ---------------- OLED ----------------
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, /*reset=*/21, /*clock=*/18, /*data=*/17);
#define VEXT_CTRL 36

// ---------------- LoRa ----------------
SX1262 radio = new Module(8, 14, 12, 13);
volatile bool receivedFlag = false;

// ---------------- WiFi AP ----------------
const char* ssid = "EcoFloat-Base";
const char* password = "12345678";

WebServer server(80);

// ---------------- Latest data ----------------
char  vesselId[16] = "";
unsigned long seqNum = 0;
char  pktDate[12] = "";
char  pktTime[10] = "";
bool  gpsFix = false;
float currentLat = 0.0f, currentLng = 0.0f, currentHdop = 99.9f;
int   currentSat = 0;
float currentDO = -1.0f, currentOS = -1.0f, currentWT = -99.0f;
bool  hasData = false;
unsigned long lastPacketMs = 0;
unsigned long packetCount = 0;

unsigned long lastDisplayMs = 0;
const unsigned long DISPLAY_INTERVAL_MS = 500;

// ---------------- Uploaded mission (KML) ----------------
// The raw KML is stored on LittleFS at /mission.kml so it survives reboots.
// The ESP32 never parses it — the browser does, with toGeoJSON.
bool hasKml = false;      // is a mission file present?
File uploadFile;          // handle used during upload streaming

// ---------------- On-device reading log ----------------
// ~44 bytes/reading x 3000 = ~132 KB static RAM. Fine on the ESP32-S3.
// At the 6 s TX interval this holds ~5 hours of survey history.
#define LOG_CAPACITY 3000

struct Reading {
  uint32_t seq;
  char     date[11];   // "2026-07-19"
  char     time[9];    // "15:03:22"
  bool     fix;
  float    lat, lng;
  uint8_t  sat;
  float    hdop;
  float    dox, os, wt;
};

Reading  logBuf[LOG_CAPACITY];
uint16_t logCount = 0;   // number of valid entries (caps at LOG_CAPACITY)
uint16_t logHead  = 0;   // next write slot

void logReading(unsigned long seq) {
  Reading &r = logBuf[logHead];
  r.seq = (uint32_t)seq;
  strncpy(r.date, pktDate, sizeof(r.date) - 1); r.date[sizeof(r.date) - 1] = '\0';
  strncpy(r.time, pktTime, sizeof(r.time) - 1); r.time[sizeof(r.time) - 1] = '\0';
  r.fix  = gpsFix;
  r.lat  = currentLat;
  r.lng  = currentLng;
  r.sat  = (uint8_t)currentSat;
  r.hdop = currentHdop;
  r.dox  = currentDO;
  r.os   = currentOS;
  r.wt   = currentWT;

  logHead = (logHead + 1) % LOG_CAPACITY;
  if (logCount < LOG_CAPACITY) logCount++;
}

#if defined(ESP8266)
  #define ISR_ATTR ICACHE_RAM_ATTR
#elif defined(ESP32)
  #define ISR_ATTR IRAM_ATTR
#else
  #define ISR_ATTR
#endif

void ISR_ATTR setFlag() { receivedFlag = true; }

// Parses: EF1,seq,date,time,lat,lng,sats,hdop,DO,sat%,temp
bool parsePacket(const String& data) {
  char id[16]; unsigned long seq; char d[12]; char t[10];
  float lat, lng, hdop, dox, os, wt; int sat;
  int n = sscanf(data.c_str(),
    "%15[^,],%lu,%11[^,],%9[^,],%f,%f,%d,%f,%f,%f,%f",
    id, &seq, d, t, &lat, &lng, &sat, &hdop, &dox, &os, &wt);
  if (n != 11) return false;

  strncpy(vesselId, id, sizeof(vesselId) - 1);
  strncpy(pktDate, d, sizeof(pktDate) - 1);
  strncpy(pktTime, t, sizeof(pktTime) - 1);
  seqNum = seq;
  gpsFix = !(lat == 0.0f && lng == 0.0f);
  if (gpsFix) {
    currentLat = lat; currentLng = lng;
  }
  currentHdop = hdop;
  currentSat = sat;
  currentDO = dox; currentOS = os; currentWT = wt;

  hasData = true;
  lastPacketMs = millis();
  packetCount++;

  logReading(seq);   // persist to on-device log
  return true;
}

// ---------------- OLED ----------------
void initOLED() {
  pinMode(VEXT_CTRL, OUTPUT);
  digitalWrite(VEXT_CTRL, LOW);
  delay(100);
  pinMode(21, OUTPUT);
  digitalWrite(21, LOW); delay(20);
  digitalWrite(21, HIGH); delay(20);
  oled.begin();
  oled.setFontPosTop();
}

void drawSplash(const char* ip) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr(0, 0,  "EcoFloat Base");
  oled.drawHLine(0, 11, 128);
  oled.drawStr(0, 16, "AP: EcoFloat-Base");
  oled.drawStr(0, 28, "IP:");
  oled.drawStr(20, 28, ip);
  oled.drawStr(0, 44, "Waiting for LoRa...");
  oled.sendBuffer();
}

void updateOLED() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr(0, 0, "EcoFloat RX");
  char rxStr[16];
  snprintf(rxStr, sizeof(rxStr), "Pkt:%lu", packetCount);
  oled.drawStr(74, 0, rxStr);
  oled.drawHLine(0, 11, 128);

  if (!hasData) {
    oled.drawStr(0, 28, "Waiting for data...");
    oled.sendBuffer();
    return;
  }

  char buf[24];
  if (gpsFix) {
    snprintf(buf, sizeof(buf), "Lat:%.5f", currentLat);
    oled.drawStr(0, 15, buf);
    snprintf(buf, sizeof(buf), "Lng:%.5f", currentLng);
    oled.drawStr(0, 26, buf);
  } else {
    oled.drawStr(0, 15, "GPS: no fix");
    snprintf(buf, sizeof(buf), "Searching, sats:%d", currentSat);
    oled.drawStr(0, 26, buf);
  }

  if (currentDO >= 0) snprintf(buf, sizeof(buf), "DO:%.2f  T:%.1fC", currentDO, currentWT);
  else                snprintf(buf, sizeof(buf), "DO:--    T:--");
  oled.drawStr(0, 37, buf);

  unsigned long ageSec = (millis() - lastPacketMs) / 1000UL;
  snprintf(buf, sizeof(buf), "Log:%u  Age:%lus", logCount, ageSec);
  oled.drawStr(0, 48, buf);

  if (ageSec < 15)      oled.drawBox(0, 60, 128, 4);
  else if (ageSec < 40) oled.drawFrame(0, 60, 128, 4);
  else                  for (int x = 0; x < 128; x += 6) oled.drawHLine(x, 62, 3);

  oled.sendBuffer();
}

// ---------------- Dashboard page ----------------
const char PAGE[] PROGMEM = R"GPSWEB(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EcoFloat live</title>
<style>
body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;margin:0;background:#0d1622;color:#e9eef5}
header{display:flex;justify-content:space-between;align-items:center;padding:12px 16px;border-bottom:1px solid #20304a}
h1{font-size:18px;margin:0}
.sub{font-size:12px;color:#8fa3c0}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:#e67e22;margin-right:6px;vertical-align:middle}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;padding:12px;max-width:900px;margin:0 auto}
.card{background:#16233a;border-radius:12px;padding:14px;text-align:center}
.lbl{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:#8fa3c0;margin-bottom:4px}
.val{font-size:30px;font-weight:700;line-height:1.1}
.unit{font-size:13px;color:#8fa3c0}
#doCard{grid-column:1/-1;padding:20px}
#doVal{font-size:56px}
#doBand{display:inline-block;margin-top:8px;padding:4px 14px;border-radius:999px;font-size:13px;font-weight:600;background:#2c3e57;color:#cfe0f5}
.section{max-width:900px;margin:0 auto;padding:0 12px 12px}
.panel{background:#16233a;border-radius:12px;padding:12px}
table{width:100%;border-collapse:collapse;font-size:12.5px}
th,td{padding:5px 6px;text-align:right;border-bottom:1px solid #20304a}
th:first-child,td:first-child{text-align:left}
button{background:#2563eb;color:#fff;border:0;border-radius:8px;padding:8px 14px;font-size:13px;cursor:pointer}
button.alt{background:#178a5b}
.btnrow{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
/* SCOPED to #trail — a bare svg{} rule here overrides Leaflet's own SVG
   renderer and silently kills the KML route polylines on the map. */
#trail{width:100%;height:300px;display:block}
#map{height:340px;border-radius:10px;background:#0a111c}
#mapNote{font-size:12px;color:#8fa3c0;margin-top:6px;display:none}
.maphdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;flex-wrap:wrap;gap:8px}
.legend{display:flex;gap:12px;font-size:11.5px;color:#8fa3c0;flex-wrap:wrap;margin-top:6px}
.sw{display:inline-block;width:10px;height:10px;border-radius:3px;margin-right:4px;vertical-align:middle}
/* --- waypoint navigation panel --- */
#navPanel{display:none;background:#16233a;border-radius:12px;padding:16px;margin:0 auto 12px;max-width:900px}
#navBig{font-size:40px;font-weight:800;line-height:1.05}
#navSub{font-size:13px;color:#8fa3c0;margin-top:4px}
.navrow{display:flex;gap:16px;align-items:center;flex-wrap:wrap}
.navstat{flex:1;min-width:120px}
.navstat .lbl{margin-bottom:2px}
.navstat .v{font-size:24px;font-weight:700}
#wpChips{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px}
.chip{padding:3px 9px;border-radius:999px;font-size:12px;background:#2c3e57;color:#cfe0f5;cursor:pointer}
.chip.active{background:#f1c40f;color:#10202e}
.chip.done{background:#178a5b;color:#dff5ea}
.arrow{font-size:34px;display:inline-block;transition:transform .3s}
</style>
</head>
<body>
<header>
<div><h1>EcoFloat live</h1><div class="sub" id="vessel">waiting for vessel...</div></div>
<div class="sub"><span class="dot" id="linkDot"></span><span id="linkTxt">connecting...</span></div>
</header>

<div class="grid">
<div class="card" id="doCard">
<div class="lbl">dissolved oxygen</div>
<div class="val" id="doVal">--</div>
<div class="unit">mg/L</div><br>
<div id="doBand">waiting for data</div>
</div>
<div class="card"><div class="lbl">saturation</div><div class="val" id="osVal">--</div><div class="unit">%</div></div>
<div class="card"><div class="lbl">water temp</div><div class="val" id="wtVal">--</div><div class="unit" id="wtF">&deg;C</div></div>
<div class="card"><div class="lbl">satellites</div><div class="val" id="satVal">--</div><div class="unit" id="fixTxt">no fix</div></div>
<div class="card"><div class="lbl">gps accuracy</div><div class="val" id="hdopVal">--</div><div class="unit" id="hdopQ">HDOP</div></div>
</div>

<!-- waypoint navigation panel -->
<div class="section"><div id="navPanel">
<div class="navrow">
<div style="text-align:center;min-width:90px">
<div class="arrow" id="navArrow">&uarr;</div>
<div class="lbl" style="margin-top:2px">relative</div>
</div>
<div style="flex:2;min-width:160px">
<div class="lbl">steer toward waypoint <span id="wpIdx">1</span></div>
<div id="navBig">--&deg;</div>
<div id="navSub">acquiring position...</div>
</div>
<div class="navstat">
<div class="lbl">distance</div><div class="v" id="navDist">--</div>
</div>
<div class="navstat">
<div class="lbl">boat heading (COG)</div><div class="v" id="navCog">--</div>
</div>
</div>
<div id="wpChips"></div>
</div></div>

<div class="section"><div class="panel">
<div class="maphdr">
<div class="lbl" style="margin:0">satellite map &middot; live position</div>
<div class="btnrow">
<label style="font-size:12px;color:#8fa3c0;cursor:pointer"><input type="checkbox" id="followChk" checked> follow</label>
<input type="file" id="kmlFile" accept=".kml" style="display:none" onchange="uploadKml(this)">
<button onclick="document.getElementById('kmlFile').click()">Upload KML</button>
<button class="alt" id="clearBtn" onclick="clearKml()" style="display:none">Clear</button>
</div>
</div>
<div id="map"></div>
<div id="mapNote">satellite map needs internet (your phone's cellular data). Still loading or no connection &mdash; live data works regardless; use the trail view below.</div>
</div></div>

<div class="section"><div class="panel">
<div class="lbl">survey trail &middot; colored by DO</div>
<svg id="trail" viewBox="0 0 600 300" preserveAspectRatio="xMidYMid meet"></svg>
<div class="legend">
<span><span class="sw" style="background:#2ecc71"></span>&ge;6 good</span>
<span><span class="sw" style="background:#f1c40f"></span>4&ndash;6 fair</span>
<span><span class="sw" style="background:#e67e22"></span>2&ndash;4 low</span>
<span><span class="sw" style="background:#e74c3c"></span>&lt;2 hypoxic</span>
</div>
</div></div>

<div class="section"><div class="panel">
<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;flex-wrap:wrap;gap:8px">
<div class="lbl" style="margin:0">readings logged: <span id="nPts">0</span> (base station: <span id="nLog">0</span>)</div>
<div class="btnrow">
<button onclick="dlCSV()">CSV (this session)</button>
<button class="alt" onclick="location.href='/csv'">Full log CSV</button>
</div>
</div>
<table><thead><tr><th>time (UTC)</th><th>lat</th><th>lng</th><th>DO</th><th>%</th><th>&deg;C</th></tr></thead>
<tbody id="rows"></tbody></table>
</div></div>

<script>
let pts=[],lastSeq=-1;

// mission state
let waypoints=[];       // [{lat,lng,name}]
let activeWp=0;         // index of the waypoint we're driving to
let wpLayer=null;       // Leaflet layer group for the drawn mission
let tgjReady=false, pendingKmlLoad=false;
let prevFix=null;       // previous GPS fix, for course-over-ground
const ARRIVE_M=8;       // waypoint considered reached within this many metres

// ---------- satellite map (loaded async so it can NEVER block the dashboard) ----------
let map=null,boatMarker=null,mapReady=false,tilesOK=false;
function loadLeaflet(){
const css=document.createElement('link');
css.rel='stylesheet';css.href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css';
document.head.appendChild(css);
const js=document.createElement('script');
js.src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js';
js.async=true;
js.onload=initMap;
js.onerror=function(){document.getElementById('mapNote').style.display='block';};
document.head.appendChild(js);
setTimeout(function(){if(!mapReady)document.getElementById('mapNote').style.display='block';},8000);
}
// load toGeoJSON (async, never blocks); when ready, load any stored mission
function loadToGeoJSON(){
const s=document.createElement('script');
s.src='https://unpkg.com/@tmcw/togeojson@5.8.1/dist/togeojson.umd.js';
s.async=true;
s.onload=function(){tgjReady=true;if(pendingKmlLoad)fetchMission();};
document.head.appendChild(s);
}
function initMap(){
if(typeof L==='undefined'){document.getElementById('mapNote').style.display='block';return;}
map=L.map('map',{zoomControl:true,attributionControl:true}).setView([41.916,-88.234],15);
const sat=L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',{maxZoom:19,attribution:'Imagery &copy; Esri'});
sat.on('tileload',function(){tilesOK=true;document.getElementById('mapNote').style.display='none';});
sat.on('tileerror',function(){if(!tilesOK)document.getElementById('mapNote').style.display='block';});
sat.addTo(map);
const boatIcon=L.divIcon({className:'',html:'<div style="width:18px;height:18px;border-radius:50%;background:#2563eb;border:3px solid #fff;box-shadow:0 0 8px rgba(0,0,0,.6)"></div>',iconSize:[18,18],iconAnchor:[9,9]});
boatMarker=L.marker([41.916,-88.234],{icon:boatIcon,zIndexOffset:1000});
mapReady=true;
document.getElementById('mapNote').style.display='none';
pts.forEach(mapAddPoint);
// draw a mission that arrived before the map finished loading
if(waypoints.length)drawMission();else{pendingKmlLoad=true;if(tgjReady)fetchMission();}
}
function mapAddPoint(p){
if(!mapReady||!p.fix)return;
if(!boatMarker._map){boatMarker.addTo(map);map.setView([p.lat,p.lng],17);}
boatMarker.setLatLng([p.lat,p.lng]);
if(p.dox>=0){L.circleMarker([p.lat,p.lng],{radius:5,color:'#0d1622',weight:1,fillColor:band(p.dox)[0],fillOpacity:.9}).bindTooltip('DO '+p.dox.toFixed(2)+' mg/L &middot; '+p.time).addTo(map);}
if(document.getElementById('followChk').checked)map.panTo([p.lat,p.lng]);
}
function band(d){return d>=6?['#2ecc71','good - most fish thrive']:d>=4?['#f1c40f','fair - sensitive fish stressed']:d>=2?['#e67e22','low - many fish stressed']:['#e74c3c','hypoxic - fish at risk'];}
function hq(h){return h<=1.5?'HDOP - excellent':h<=2.5?'HDOP - good':h<=5?'HDOP - fair':'HDOP - poor';}
function fmt(v,n){return (v==null||v<0)?'--':v.toFixed(n);}

// ---------- navigation math ----------
function toRad(d){return d*Math.PI/180;}
function toDeg(r){return r*180/Math.PI;}
function distM(a,b){
const R=6371000,p1=toRad(a.lat),p2=toRad(b.lat);
const dp=toRad(b.lat-a.lat),dl=toRad(b.lng-a.lng);
const x=Math.sin(dp/2)*Math.sin(dp/2)+Math.cos(p1)*Math.cos(p2)*Math.sin(dl/2)*Math.sin(dl/2);
return 2*R*Math.asin(Math.sqrt(x));
}
function bearing(a,b){
const p1=toRad(a.lat),p2=toRad(b.lat),dl=toRad(b.lng-a.lng);
const y=Math.sin(dl)*Math.cos(p2);
const x=Math.cos(p1)*Math.sin(p2)-Math.sin(p1)*Math.cos(p2)*Math.cos(dl);
return (toDeg(Math.atan2(y,x))+360)%360;
}
function compass(deg){
const d=['N','NNE','NE','ENE','E','ESE','SE','SSE','S','SSW','SW','WSW','W','WNW','NW','NNW'];
return d[Math.round(deg/22.5)%16];
}
function fmtDist(m){return m>=1000?(m/1000).toFixed(2)+' km':m.toFixed(0)+' m';}

// ---------- mission upload / clear ----------
function uploadKml(input){
const f=input.files[0];if(!f)return;
const fd=new FormData();fd.append('file',f);
fetch('/upload',{method:'POST',body:fd})
.then(function(){pendingKmlLoad=true;if(tgjReady)fetchMission();})
.catch(function(){alert('upload failed - still connected to EcoFloat-Base?');});
input.value='';
}
function clearKml(){
fetch('/clearkml').then(function(){
waypoints=[];activeWp=0;
if(wpLayer&&map){map.removeLayer(wpLayer);wpLayer=null;}
document.getElementById('navPanel').style.display='none';
document.getElementById('clearBtn').style.display='none';
document.getElementById('wpChips').innerHTML='';
});
}

// ---------- fetch stored KML, parse in the browser ----------
function fetchMission(){
pendingKmlLoad=false;
fetch('/kml').then(function(r){if(!r.ok)throw 0;return r.text();})
.then(function(txt){
const dom=new DOMParser().parseFromString(txt,'text/xml');
const gj=toGeoJSON.kml(dom);
waypoints=[];
gj.features.forEach(function(ft){
const g=ft.geometry;if(!g)return;
const nm=(ft.properties&&ft.properties.name)||'';
if(g.type==='Point'){
waypoints.push({lng:g.coordinates[0],lat:g.coordinates[1],name:nm});
}else if(g.type==='LineString'){
g.coordinates.forEach(function(c,i){waypoints.push({lng:c[0],lat:c[1],name:nm?(nm+' '+(i+1)):''});});
}
});
activeWp=0;
drawMission();
if(waypoints.length){
document.getElementById('navPanel').style.display='block';
document.getElementById('clearBtn').style.display='inline-block';
}
})
.catch(function(){/* no mission yet - fine */});
}

// ---------- draw waypoints + route on the map ----------
function drawMission(){
if(!mapReady)return;
if(wpLayer)map.removeLayer(wpLayer);
wpLayer=L.layerGroup().addTo(map);
const latlngs=waypoints.map(function(w){return [w.lat,w.lng];});
if(latlngs.length>1)L.polyline(latlngs,{color:'#f1c40f',weight:2,dashArray:'6 5',opacity:.9}).addTo(wpLayer);
waypoints.forEach(function(w,i){
const ic=L.divIcon({className:'',html:'<div style="width:22px;height:22px;border-radius:50%;background:'+(i===activeWp?'#f1c40f':'#2c3e57')+';border:2px solid #fff;color:'+(i===activeWp?'#10202e':'#fff')+';font:700 12px sans-serif;display:flex;align-items:center;justify-content:center">'+(i+1)+'</div>',iconSize:[22,22],iconAnchor:[11,11]});
L.marker([w.lat,w.lng],{icon:ic}).bindTooltip('WP'+(i+1)+(w.name?' - '+w.name:'')).addTo(wpLayer);
});
if(latlngs.length&&map)map.fitBounds(L.latLngBounds(latlngs).pad(0.3));
drawChips();
}
function drawChips(){
document.getElementById('wpChips').innerHTML=waypoints.map(function(w,i){
const cls=i<activeWp?'chip done':(i===activeWp?'chip active':'chip');
return '<span class="'+cls+'" onclick="setActive('+i+')">WP'+(i+1)+'</span>';
}).join('');
}
function setActive(i){activeWp=i;drawMission();}

// ---------- nav update, called on each new fix ----------
function updateNav(lat,lng){
if(!waypoints.length||activeWp>=waypoints.length)return;
const boat={lat:lat,lng:lng};
const tgt=waypoints[activeWp];
const d=distM(boat,tgt);
const brg=bearing(boat,tgt);

if(d<=ARRIVE_M){
activeWp++;
drawMission();
if(activeWp>=waypoints.length){
document.getElementById('navBig').textContent='DONE';
document.getElementById('navSub').textContent='all waypoints reached';
document.getElementById('navDist').textContent='--';
document.getElementById('navArrow').style.transform='';
return;
}
return; // recompute against the new target next tick
}

let cog=null;
if(prevFix&&distM(prevFix,boat)>1.5){cog=bearing(prevFix,boat);}

document.getElementById('wpIdx').textContent=(activeWp+1);
document.getElementById('navBig').textContent=brg.toFixed(0)+'\u00b0 '+compass(brg);
document.getElementById('navDist').textContent=fmtDist(d);
document.getElementById('navCog').textContent=(cog==null)?'--':(cog.toFixed(0)+'\u00b0');

if(cog!=null){
let rel=((brg-cog+540)%360)-180; // -180..+180, + = turn right
document.getElementById('navArrow').style.transform='rotate('+rel+'deg)';
document.getElementById('navSub').textContent=
(Math.abs(rel)<10?'on course':(rel>0?'turn right '+Math.abs(rel).toFixed(0)+'\u00b0':'turn left '+Math.abs(rel).toFixed(0)+'\u00b0'));
}else{
document.getElementById('navArrow').style.transform='rotate('+brg+'deg)';
document.getElementById('navSub').textContent='move to get course; arrow shows compass bearing';
}
}

async function tick(){
try{const j=await (await fetch('/data')).json();render(j);}
catch(e){document.getElementById('linkTxt').textContent='no connection';document.getElementById('linkDot').style.background='#e74c3c';}
}
function render(j){
const dot=document.getElementById('linkDot'),lt=document.getElementById('linkTxt');
if(!j.hasData){lt.textContent='waiting for first packet';dot.style.background='#e67e22';return;}
document.getElementById('vessel').textContent='vessel '+j.id+' \u00b7 '+j.date+' '+j.time+' UTC \u00b7 pkt '+j.seq;
dot.style.background=j.ageSec<25?'#2ecc71':j.ageSec<60?'#f1c40f':'#e74c3c';
lt.textContent='updated '+j.ageSec+'s ago';
const hasDO=j.do>=0;
document.getElementById('doVal').textContent=hasDO?j.do.toFixed(2):'--';
const b=document.getElementById('doBand');
if(hasDO){const r=band(j.do);b.style.background=r[0];b.style.color='#10202e';b.textContent=r[1];}
else{b.style.background='#2c3e57';b.style.color='#cfe0f5';b.textContent='sensor offline';}
document.getElementById('osVal').textContent=fmt(j.os,1);
document.getElementById('wtVal').textContent=(j.wt>-50)?j.wt.toFixed(2):'--';
document.getElementById('wtF').innerHTML=(j.wt>-50)?('&deg;C \u00b7 '+(j.wt*9/5+32).toFixed(1)+'&deg;F'):'&deg;C';
document.getElementById('satVal').textContent=j.sat;
document.getElementById('fixTxt').textContent=j.fix?'fix OK':'no fix';
document.getElementById('hdopVal').textContent=j.fix?j.hdop.toFixed(1):'--';
document.getElementById('hdopQ').textContent=j.fix?hq(j.hdop):'HDOP';
document.getElementById('nLog').textContent=j.logN;
if(j.seq!==lastSeq){
lastSeq=j.seq;
pts.push({seq:j.seq,date:j.date,time:j.time,fix:j.fix,lat:j.lat,lng:j.lng,sat:j.sat,hdop:j.hdop,dox:j.do,os:j.os,wt:j.wt});
if(pts.length>2000)pts.shift();
mapAddPoint(pts[pts.length-1]);
if(j.fix){updateNav(j.lat,j.lng);prevFix={lat:j.lat,lng:j.lng};}
drawTrail();drawTable();
}
}
function drawTable(){
document.getElementById('nPts').textContent=pts.length;
document.getElementById('rows').innerHTML=pts.slice(-8).reverse().map(function(p){
return '<tr><td>'+p.time+'</td><td>'+(p.fix?p.lat.toFixed(5):'--')+'</td><td>'+(p.fix?p.lng.toFixed(5):'--')+'</td><td>'+fmt(p.dox,2)+'</td><td>'+fmt(p.os,1)+'</td><td>'+((p.wt>-50)?p.wt.toFixed(2):'--')+'</td></tr>';
}).join('');
}
function drawTrail(){
const f=pts.filter(function(p){return p.fix&&p.dox>=0;});
const s=document.getElementById('trail');
if(f.length<1){s.innerHTML='<text x="300" y="150" fill="#8fa3c0" font-size="13" text-anchor="middle">waiting for GPS fix + DO reading...</text>';return;}
const la=f.map(function(p){return p.lat;}),lo=f.map(function(p){return p.lng;});
const la0=Math.min.apply(null,la),la1=Math.max.apply(null,la);
const lo0=Math.min.apply(null,lo),lo1=Math.max.apply(null,lo);
const k=Math.cos((la0+la1)/2*Math.PI/180);
let dx=(lo1-lo0)*k,dy=la1-la0;
if(dx<1e-7)dx=1e-7;if(dy<1e-7)dy=1e-7;
const sc=Math.min(540/dx,260/dy);
function X(p){return 300+((p.lng-(lo0+lo1)/2)*k)*sc;}
function Y(p){return 150-((p.lat-(la0+la1)/2))*sc;}
let h='';
if(f.length>1)h+='<polyline fill="none" stroke="#3b5a86" stroke-width="1.5" points="'+f.map(function(p){return X(p).toFixed(1)+','+Y(p).toFixed(1);}).join(' ')+'"/>';
f.forEach(function(p,i){
const last=(i===f.length-1);
h+='<circle cx="'+X(p).toFixed(1)+'" cy="'+Y(p).toFixed(1)+'" r="'+(last?7:4)+'" fill="'+band(p.dox)[0]+'"'+(last?' stroke="#fff" stroke-width="2"':'')+'/>';
});
s.innerHTML=h;
}
function dlCSV(){
let c='seq,date,time_utc,lat,lng,sats,hdop,do_mgL,sat_pct,temp_C\n';
pts.forEach(function(p){
c+=[p.seq,p.date,p.time,(p.fix?p.lat.toFixed(6):''),(p.fix?p.lng.toFixed(6):''),p.sat,p.hdop,(p.dox>=0?p.dox:''),(p.os>=0?p.os:''),((p.wt>-50)?p.wt:'')].join(',')+'\n';
});
const a=document.createElement('a');
a.href=URL.createObjectURL(new Blob([c],{type:'text/csv'}));
a.download='ecofloat_survey.csv';
a.click();
}
setInterval(tick,2000);tick();
loadLeaflet();
loadToGeoJSON();
</script>
</body>
</html>)GPSWEB";

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleData() {
  unsigned long ageSec = hasData ? (millis() - lastPacketMs) / 1000UL : 0UL;
  String json = "{";
  json += "\"hasData\":"; json += (hasData ? "true" : "false");
  json += ",\"id\":\"";   json += vesselId; json += "\"";
  json += ",\"seq\":";    json += String(seqNum);
  json += ",\"date\":\""; json += pktDate;  json += "\"";
  json += ",\"time\":\""; json += pktTime;  json += "\"";
  json += ",\"fix\":";    json += (gpsFix ? "true" : "false");
  json += ",\"lat\":";    json += String(currentLat, 6);
  json += ",\"lng\":";    json += String(currentLng, 6);
  json += ",\"sat\":";    json += String(currentSat);
  json += ",\"hdop\":";   json += String(currentHdop, 1);
  json += ",\"do\":";     json += String(currentDO, 2);
  json += ",\"os\":";     json += String(currentOS, 1);
  json += ",\"wt\":";     json += String(currentWT, 2);
  json += ",\"ageSec\":"; json += String(ageSec);
  json += ",\"logN\":";   json += String(logCount);
  json += ",\"hasKml\":"; json += (hasKml ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

// Stream the full on-device log as a CSV download.
// Chunked transfer so we never need the whole file in RAM at once.
void handleCsv() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-Disposition", "attachment; filename=ecofloat_full_log.csv");
  server.send(200, "text/csv", "");
  server.sendContent("seq,date,time_utc,lat,lng,sats,hdop,do_mgL,sat_pct,temp_C\n");

  String chunk;
  chunk.reserve(2200);

  // Oldest entry first: if the ring has wrapped, oldest is at logHead.
  uint16_t start = (logCount == LOG_CAPACITY) ? logHead : 0;

  for (uint16_t i = 0; i < logCount; i++) {
    const Reading &r = logBuf[(start + i) % LOG_CAPACITY];
    char line[140];
    char latStr[16] = "", lngStr[16] = "", doStr[12] = "", osStr[12] = "", wtStr[12] = "";
    if (r.fix)       { snprintf(latStr, sizeof(latStr), "%.6f", r.lat);
                       snprintf(lngStr, sizeof(lngStr), "%.6f", r.lng); }
    if (r.dox >= 0)  snprintf(doStr, sizeof(doStr), "%.2f", r.dox);
    if (r.os  >= 0)  snprintf(osStr, sizeof(osStr), "%.1f", r.os);
    if (r.wt  > -50) snprintf(wtStr, sizeof(wtStr), "%.2f", r.wt);

    snprintf(line, sizeof(line), "%lu,%s,%s,%s,%s,%u,%.1f,%s,%s,%s\n",
             (unsigned long)r.seq, r.date, r.time, latStr, lngStr,
             (unsigned)r.sat, r.hdop, doStr, osStr, wtStr);
    chunk += line;

    if (chunk.length() > 1800) {
      server.sendContent(chunk);
      chunk = "";
    }
  }
  if (chunk.length()) server.sendContent(chunk);
  server.sendContent("");   // terminate chunked response
}

// Serve the stored KML back to the browser for client-side parsing.
void handleGetKml() {
  if (!LittleFS.exists("/mission.kml")) {
    server.send(404, "text/plain", "no mission uploaded");
    return;
  }
  File f = LittleFS.open("/mission.kml", "r");
  server.streamFile(f, "application/vnd.google-earth.kml+xml");
  f.close();
}

// Receive a KML upload in chunks and write it to LittleFS.
void handleUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (LittleFS.exists("/mission.kml")) LittleFS.remove("/mission.kml");
    uploadFile = LittleFS.open("/mission.kml", "w");
    Serial.print("Upload start: "); Serial.println(up.filename);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    hasKml = true;
    Serial.print("Upload done, bytes: "); Serial.println(up.totalSize);
  }
}

// Delete the stored mission.
void handleClearKml() {
  if (LittleFS.exists("/mission.kml")) LittleFS.remove("/mission.kml");
  hasKml = false;
  server.send(200, "text/plain", "cleared");
}

// ---------------- Setup / Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("EcoFloat base station starting...");

  initOLED();
  drawSplash("starting...");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  String ipStr = WiFi.softAPIP().toString();
  Serial.print("AP IP: "); Serial.println(ipStr);
  drawSplash(ipStr.c_str());

  // mount LittleFS and note whether a mission already exists
  if (!LittleFS.begin(true)) {          // true = format on first use
    Serial.println("LittleFS mount failed");
  } else {
    hasKml = LittleFS.exists("/mission.kml");
    Serial.print("LittleFS ok, mission present: ");
    Serial.println(hasKml ? "yes" : "no");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/csv", handleCsv);
  server.on("/kml", HTTP_GET, handleGetKml);
  server.on("/upload", HTTP_POST,
            [](){ server.send(200, "text/plain", "OK"); },
            handleUpload);
  server.on("/clearkml", HTTP_GET, handleClearKml);
  server.begin();

  SPI.begin(9, 11, 10, 8);
  // MUST mirror boat v3: SF9, CR 4/7, sync 0x12, preamble 8, TCXO 1.6V
  int state = radio.begin(915.0, 125.0, 9, 7, 0x12, 14, 8, 1.6, false);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print("LoRa init failed, code: "); Serial.println(state);
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tr);
    oled.drawStr(0, 0, "LoRa init FAIL");
    char errBuf[16];
    snprintf(errBuf, sizeof(errBuf), "Code: %d", state);
    oled.drawStr(0, 16, errBuf);
    oled.sendBuffer();
    while (true) { delay(100); }
  }

  radio.setDio2AsRfSwitch(true);
  radio.setDio1Action(setFlag);
  radio.startReceive();
  Serial.println("LoRa RX ready.");
}

void loop() {
  server.handleClient();

  bool packetReady = false;
  noInterrupts();
  if (receivedFlag) { receivedFlag = false; packetReady = true; }
  interrupts();

  if (packetReady) {
    String data;
    int state = radio.readData(data);
    if (state == RADIOLIB_ERR_NONE) {
      Serial.print("Received: "); Serial.println(data);
      if (!parsePacket(data)) Serial.println("Invalid packet format.");
    } else {
      Serial.print("LoRa read error: "); Serial.println(state);
    }
    radio.startReceive();
  }

  if (millis() - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
    lastDisplayMs = millis();
    updateOLED();
  }
}
