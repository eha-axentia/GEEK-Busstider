#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "wifi_credentials.h"

// Waveshare ESP32-C6-GEEK: ST7789 240x135, SPI
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789  _panel;
    lgfx::Bus_SPI       _bus;
    lgfx::Light_PWM     _light;

public:
    LGFX() {
        {
            auto cfg = _bus.config();
            cfg.spi_host   = SPI2_HOST;
            cfg.spi_mode   = 3;
            cfg.freq_write = 40000000;
            cfg.freq_read  = 16000000;
            cfg.pin_sclk   = 1;
            cfg.pin_mosi   = 2;
            cfg.pin_miso   = -1;
            cfg.pin_dc     = 3;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs      = 5;
            cfg.pin_rst     = 4;
            cfg.pin_busy    = -1;
            cfg.memory_width  = 240;
            cfg.memory_height = 320;
            cfg.panel_width   = 135;
            cfg.panel_height  = 240;
            cfg.offset_x      = 52;
            cfg.offset_y      = 40;
            cfg.offset_rotation = 0;
            cfg.invert      = true;
            cfg.rgb_order   = false;
            cfg.dlen_16bit  = false;
            cfg.bus_shared  = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl      = 6;
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = 0;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

static LGFX        lcd;
static WebServer   server(80);
static Preferences prefs;
static bool        serverActive  = false;
static bool        wifiConnected = false;

static const char AP_SSID[] = "ESP32-C6-GEEK";

// Forward declarations
void startWebServer();
void handleSettings();

// URL-encode a string for use in a URL path segment
static String urlEncode(const String& s) {
    String out;
    out.reserve(s.length() * 3);
    for (size_t i = 0; i < s.length(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Read API key from NVS, falling back to the compile-time default
static String getApiKey() {
    prefs.begin("app", true);
    String key = prefs.getString("apikey", TRAFIKLAB_API_KEY);
    prefs.end();
    return key;
}

static String getMapboxKey() {
    prefs.begin("app", true);
    String key = prefs.getString("mapboxkey", "");
    prefs.end();
    return key;
}

// Proxy a GET request to the Trafiklab API and forward the response
static void proxyGet(const String& url) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        server.send(200, "application/json", http.getString());
    } else if (code == 401 || code == 403) {
        server.send(200, "application/json",
            "{\"error\":\"API key invalid or missing (HTTP " + String(code) + ")\","
            "\"api_key_invalid\":true}");
    } else {
        server.send(502, "application/json",
            "{\"error\":\"API returned " + String(code) + "\"}");
    }
    http.end();
}

// Proxy a Mapbox Static Image and stream it back as chunked binary
static void proxyMapImage(const String& url) {
    WiFiClientSecure tlsClient;
    tlsClient.setInsecure();
    HTTPClient http;
    http.begin(tlsClient, url);
    http.setTimeout(15000);
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        WiFiClient* stream = http.getStreamPtr();
        int remaining = http.getSize(); // -1 if unknown
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "image/jpeg", "");
        uint8_t buf[512];
        while (http.connected() && (remaining != 0)) {
            size_t avail = stream->available();
            if (avail) {
                size_t toRead = min(avail, sizeof(buf));
                if (remaining > 0) toRead = min(toRead, (size_t)remaining);
                size_t n = stream->readBytes(buf, toRead);
                if (n) {
                    server.sendContent((const char*)buf, n);
                    if (remaining > 0) remaining -= n;
                }
            } else {
                delay(1);
            }
        }
        server.sendContent("", 0);
    } else if (code == 401 || code == 403) {
        server.send(403, "text/plain", "Mapbox key invalid");
    } else {
        server.send(502, "text/plain", "Mapbox error: " + String(code));
    }
    http.end();
}

// ---------------------------------------------------------------------------
// Main page (STA mode default)
// ---------------------------------------------------------------------------
static const char MAIN_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Busstider</title>
<style>
body{font-family:sans-serif;max-width:540px;margin:20px auto;padding:0 16px}
h2{margin-bottom:8px}
.topbar{display:flex;justify-content:space-between;align-items:baseline;
        font-size:13px;color:#666;margin-bottom:12px}
#clock{font-size:20px;font-weight:bold;color:#222}
.row{display:flex;gap:8px;margin-bottom:10px}
input[type=text]{flex:1;padding:8px;border:1px solid #ccc;border-radius:4px;font-size:16px}
button{padding:8px 14px;background:#0070f3;color:#fff;border:none;border-radius:4px;font-size:16px;cursor:pointer}
select{width:100%;padding:8px;margin-bottom:10px;border:1px solid #ccc;border-radius:4px;font-size:15px;display:none}
#stopinfo{margin-bottom:8px}
#stopinfo img{width:100%;border-radius:6px;display:block;margin-bottom:4px}
.coords{font-size:12px;color:#888;margin-bottom:6px}
#updated{font-size:12px;color:#aaa;margin-bottom:6px;min-height:1em}
#msg{font-size:14px;color:#888;min-height:1.4em;margin:0 0 8px}
table{width:100%;border-collapse:collapse;font-size:14px}
th{background:#f0f0f0;text-align:left;padding:6px 8px;border-bottom:2px solid #ccc}
td{padding:6px 8px;border-bottom:1px solid #eee}
tr.cancel td{color:red;font-weight:bold}
tr.late td{color:orange}
footer{margin-top:20px;font-size:13px}
footer a{color:#555;text-decoration:none}
</style></head>
<body>
<h2>Busstider</h2>
<div class="topbar">
  <span id="clock"></span>
  <span id="updated"></span>
</div>
<div class="row">
  <input id="q" type="text" placeholder="Stop name&hellip;" autocomplete="off"
         onkeydown="if(event.key==='Enter')search()">
  <button onclick="search()">Search</button>
</div>
<p id="msg"></p>
<select id="sel" onchange="onStopSelect(this.value)"></select>
<div id="stopinfo"></div>
<div id="deps"></div>
<footer><a href="/settings">&#9881; WiFi Settings</a></footer>
<script>
const msg      = document.getElementById('msg');
const sel      = document.getElementById('sel');
const deps     = document.getElementById('deps');
const stopinfo = document.getElementById('stopinfo');
const updated  = document.getElementById('updated');

let stopMeta   = {};   // id -> { name, lat, lon }
let currentId  = null;
let refreshTmr = null;

// Clock
setInterval(()=>{
  document.getElementById('clock').textContent =
    new Date().toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'});
}, 1000);

async function search(){
  const q=document.getElementById('q').value.trim();
  if(!q)return;
  msg.textContent='Searching\u2026';
  sel.style.display='none';
  deps.innerHTML='';
  stopinfo.textContent='';
  updated.textContent='';
  clearInterval(refreshTmr);
  currentId=null;
  try{
    const r=await fetch('/api/stops?q='+encodeURIComponent(q));
    const d=await r.json();
    if(d.api_key_invalid){msg.innerHTML='API key invalid. <a href="/settings">Update in Settings</a>.';return;}
    if(d.error){msg.textContent='Error: '+d.error;return;}
    const g=(d.stop_groups||[]).filter(s=>s.transport_modes&&s.transport_modes.length>0);
    if(!g.length){msg.textContent='No stops found.';return;}
    stopMeta={};
    sel.innerHTML='<option value="">— select a stop —</option>';
    g.forEach(s=>{
      // Average lat/lon across child stops
      const pts=(s.stops||[]).filter(p=>p.lat!=null);
      const lat=pts.length?pts.reduce((a,p)=>a+p.lat,0)/pts.length:null;
      const lon=pts.length?pts.reduce((a,p)=>a+p.lon,0)/pts.length:null;
      stopMeta[s.id]={name:s.name, lat, lon};
      const modes=s.transport_modes&&s.transport_modes.length
        ?' ['+s.transport_modes.join(', ')+']':'';
      sel.add(new Option(s.name+modes, s.id));
    });
    sel.style.display='block';
    msg.textContent=g.length+' stop'+(g.length!==1?'s':'')+' found.';
    if(g.length===1){sel.value=g[0].id;onStopSelect(g[0].id);}
  }catch(e){msg.textContent='Error: '+e.message;}
}

function onStopSelect(id){
  clearInterval(refreshTmr);
  currentId=id;
  if(!id){stopinfo.innerHTML='';return;}
  const m=stopMeta[id];
  if(m&&m.lat!=null){
    stopinfo.innerHTML=
      `<img src="/api/map?lat=${m.lat}&lon=${m.lon}" alt="Map">`+
      `<div class="coords">\uD83D\uDCCD ${m.lat.toFixed(5)}, ${m.lon.toFixed(5)}</div>`;
  } else {
    stopinfo.innerHTML='';
  }
  loadDeps(id);
  refreshTmr=setInterval(()=>loadDeps(currentId), 60000);
}

function fmt(iso){return iso?iso.slice(11,16):'\u2014';}
function fmtDelay(s){
  if(s==null||s===0)return '';
  const m=Math.round(s/60);
  return (m>0?'+':'')+m+'min';
}

async function loadDeps(id){
  if(!id)return;
  try{
    const r=await fetch('/api/departures?stopId='+id);
    const d=await r.json();
    if(d.api_key_invalid){deps.innerHTML='<p>API key invalid. <a href="/settings">Update in Settings</a>.</p>';clearInterval(refreshTmr);return;}
    if(d.error){deps.innerHTML='<p>Error: '+d.error+'</p>';return;}
    updated.textContent='Updated: '+new Date().toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'});
    const list=d.departures||[];
    if(!list.length){deps.innerHTML='<p>No departures found.</p>';return;}
    let h='<table><thead><tr>'
      +'<th>Line</th><th>Direction</th>'
      +'<th>Sched</th><th>Real</th><th>Delay</th><th>Track</th>'
      +'</tr></thead><tbody>';
    list.forEach(dep=>{
      const ro=dep.route||{};
      const line=(ro.designation||'\u2014')
        +(ro.transport_mode?' \u00b7 '+ro.transport_mode:'');
      const dir=ro.direction||'\u2014';
      const platform=dep.scheduled_platform&&dep.scheduled_platform.designation
        ?dep.scheduled_platform.designation:'\u2014';
      const late=dep.delay>60;
      const rowCls=dep.canceled?'cancel':late?'late':'';
      h+=`<tr class="${rowCls}">
        <td>${line}</td>
        <td>${dir}</td>
        <td>${fmt(dep.scheduled)}</td>
        <td>${fmt(dep.realtime)}</td>
        <td>${fmtDelay(dep.delay)}</td>
        <td>${platform}</td>
      </tr>`;
    });
    h+='</tbody></table>';
    deps.innerHTML=h;
  }catch(e){deps.innerHTML='<p>Error: '+e.message+'</p>';}
}
</script>
</body></html>)html";

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------

void handleRoot() {
    if (wifiConnected) {
        server.send_P(200, "text/html", MAIN_HTML);
    } else {
        handleSettings();
    }
}

// Settings page — WiFi credential form with scanned SSID dropdown + API key
void handleSettings() {
    int n = WiFi.scanNetworks();

    String html =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Settings</title><style>"
        "body{font-family:sans-serif;max-width:380px;margin:40px auto;padding:0 16px}"
        "h2{margin-bottom:4px}"
        "h3{margin:20px 0 8px;font-size:15px;color:#333;border-top:1px solid #eee;padding-top:16px}"
        "label{display:block;margin-bottom:4px;font-size:14px;color:#555}"
        "input{display:block;width:100%;padding:8px;margin-bottom:16px;"
        "border:1px solid #ccc;border-radius:4px;box-sizing:border-box;font-size:16px}"
        "button{width:100%;padding:10px;background:#0070f3;color:#fff;"
        "border:none;border-radius:4px;font-size:16px;cursor:pointer;margin-bottom:8px}"
        "a{display:block;margin-top:16px;font-size:13px;color:#555;text-decoration:none}"
        "</style></head><body>"
        "<h2>Settings</h2>";

    if (wifiConnected) {
        html += "<a href=\"/\">\u2190 Back</a>";
    }

    // WiFi section
    html +=
        "<h3>WiFi</h3>"
        "<form method=\"POST\" action=\"/save\">"
        "<label>SSID</label>"
        "<input type=\"text\" name=\"ssid\" list=\"networks\" autocomplete=\"off\" required>"
        "<datalist id=\"networks\">";

    for (int i = 0; i < n; i++) {
        html += "<option value=\"" + WiFi.SSID(i) + "\">";
    }
    WiFi.scanDelete();

    html +=
        "</datalist>"
        "<label>Password</label>"
        "<input type=\"password\" name=\"password\" autocomplete=\"off\">"
        "<button type=\"submit\">Save &amp; Connect</button>"
        "</form>";

    // API key section
    html +=
        "<h3>Trafiklab API Key</h3>"
        "<form method=\"POST\" action=\"/savekey\">"
        "<label>API Key</label>"
        "<input type=\"text\" name=\"apikey\""
        " autocomplete=\"off\" spellcheck=\"false\" placeholder=\"Enter new API key\">"
        "<button type=\"submit\">Save API Key</button>"
        "</form>"
        "<h3>Mapbox API Key</h3>"
        "<form method=\"POST\" action=\"/savemapboxkey\">"
        "<label>API Key</label>"
        "<input type=\"text\" name=\"mapboxkey\""
        " autocomplete=\"off\" spellcheck=\"false\" placeholder=\"Enter Mapbox API key\">"
        "<button type=\"submit\">Save Mapbox Key</button>"
        "</form>"
        "</body></html>";

    server.send(200, "text/html", html);
}

void handleSave() {
    if (!server.hasArg("ssid") || server.arg("ssid").isEmpty()) {
        server.send(400, "text/plain", "SSID is required.");
        return;
    }

    prefs.begin("wifi", false);
    prefs.putString("ssid",     server.arg("ssid"));
    prefs.putString("password", server.arg("password"));
    prefs.end();

    server.send(200, "text/html",
        "<!DOCTYPE html><html><body>"
        "<p>Saved! Restarting in 2 seconds...</p>"
        "</body></html>");
    delay(2000);
    ESP.restart();
}

void handleSaveKey() {
    if (!server.hasArg("apikey") || server.arg("apikey").isEmpty()) {
        server.send(400, "text/plain", "API key is required.");
        return;
    }

    prefs.begin("app", false);
    prefs.putString("apikey", server.arg("apikey"));
    prefs.end();

    server.sendHeader("Location", wifiConnected ? "/" : "/settings");
    server.send(303);
}

void handleSaveMapboxKey() {
    if (!server.hasArg("mapboxkey") || server.arg("mapboxkey").isEmpty()) {
        server.send(400, "text/plain", "Mapbox key is required.");
        return;
    }
    prefs.begin("app", false);
    prefs.putString("mapboxkey", server.arg("mapboxkey"));
    prefs.end();
    server.sendHeader("Location", wifiConnected ? "/" : "/settings");
    server.send(303);
}

void handleApiMap() {
    String lat = server.arg("lat");
    String lon = server.arg("lon");
    if (lat.isEmpty() || lon.isEmpty()) {
        server.send(400, "text/plain", "lat and lon required");
        return;
    }
    String key = getMapboxKey();
    if (key.isEmpty()) {
        server.send(503, "text/plain", "Mapbox key not configured. Add it in Settings.");
        return;
    }
    String url =
        String("https://api.mapbox.com/styles/v1/mapbox/streets-v11/static/")
        + "pin-m+e74c3c(" + lon + "," + lat + ")/"
        + lon + "," + lat + ",15/500x280?access_token=" + key;
    proxyMapImage(url);
}

void handleApiStops() {
    String q = server.arg("q");
    if (q.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"q parameter required\"}");
        return;
    }
    proxyGet(String("https://realtime-api.trafiklab.se/v1/stops/name/")
             + urlEncode(q) + "?key=" + getApiKey());
}

void handleApiDepartures() {
    String stopId = server.arg("stopId");
    if (stopId.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"stopId parameter required\"}");
        return;
    }
    proxyGet(String("https://realtime-api.trafiklab.se/v1/departures/")
             + stopId + "?key=" + getApiKey());
}

// ---------------------------------------------------------------------------
// WiFi / server startup
// ---------------------------------------------------------------------------

bool connectWifi() {
    prefs.begin("wifi", true);
    String ssid     = prefs.getString("ssid",     WIFI_SSID);
    String password = prefs.getString("password", WIFI_PASSWORD);
    prefs.end();

    lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    lcd.setTextSize(1);
    lcd.setCursor(10, 55);
    lcd.print("WiFi: connecting...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());
    Serial.print("Connecting to WiFi");

    for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        wifiConnected = true;
        lcd.setCursor(10, 55);
        lcd.setTextColor(TFT_GREEN, TFT_BLACK);
        lcd.printf("WiFi: %-20s", ssid.c_str());
        lcd.setCursor(10, 68);
        lcd.print("IP: ");
        lcd.print(WiFi.localIP().toString().c_str());
        Serial.println("Connected! IP: " + WiFi.localIP().toString());
        startWebServer();
        return true;
    }

    lcd.setCursor(10, 55);
    lcd.setTextColor(TFT_RED, TFT_BLACK);
    lcd.print("WiFi: FAILED        ");
    Serial.println("WiFi connection failed!");
    return false;
}

void startWebServer() {
    server.on("/",                HTTP_GET,  handleRoot);
    server.on("/settings",        HTTP_GET,  handleSettings);
    server.on("/save",            HTTP_POST, handleSave);
    server.on("/savekey",         HTTP_POST, handleSaveKey);
    server.on("/savemapboxkey",   HTTP_POST, handleSaveMapboxKey);
    server.on("/api/stops",       HTTP_GET,  handleApiStops);
    server.on("/api/departures",  HTTP_GET,  handleApiDepartures);
    server.on("/api/map",         HTTP_GET,  handleApiMap);
    server.begin();
    serverActive = true;
}

void startConfigAP() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);

    lcd.setTextColor(TFT_ORANGE, TFT_BLACK);
    lcd.setTextSize(1);
    lcd.setCursor(10, 55);
    lcd.printf("AP: %-21s", AP_SSID);
    lcd.setCursor(10, 68);
    lcd.print("-> 192.168.4.1      ");

    Serial.printf("AP started: %s\n", AP_SSID);
    Serial.println("Open http://192.168.4.1 to configure WiFi.");

    startWebServer();
}

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);

    lcd.init();
    lcd.setBrightness(255);
    lcd.setRotation(1);      // landscape, USB-C at left
    lcd.fillScreen(TFT_BLACK);

    lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    lcd.setTextSize(2);
    lcd.setCursor(10, 5);
    lcd.println("ESP32-C6-GEEK");

    lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    lcd.setTextSize(1);
    lcd.setCursor(10, 35);
    lcd.println("PlatformIO ready");

    Serial.println("Display initialised.");

    if (!connectWifi()) {
        startConfigAP();
    }
}

void loop() {
    if (serverActive) {
        server.handleClient();
    }
}
