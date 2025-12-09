/*
  PILAB GPS + RFID + Firebase + MOSFET control

  - iOS-style setup UI + captive portal
  - Config in EEPROM
  - GPS + RFID data to Firebase
  - RFID check (1 GET + 1 PUT to rfid_data.json)
  - MOSFET on GPIO5:
      * If scanned UID == uid1 -> MOSFET ON (GPIO5 HIGH)
      * If scanned UID == uid2 -> MOSFET OFF (GPIO5 LOW)

  On first config upload, NVS/config is sent ONE BY ONE to:
    /data/<veh>/rtmp1
    /data/<veh>/rtmp2
    /data/<veh>/rtmp3
    /data/<veh>/rtmp4
    /data/<veh>/imei
    /data/<veh>/vehnum
    /data/<veh>/timestampz
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <DNSServer.h>

#include <TinyGPSPlus.h>
#include <SPI.h>
#include <MFRC522.h>

/* ---------------- LED / Button / Buzzer / MOSFET ---------------- */

#define PIN_R      10     // Red LED
#define PIN_G      16     // Green LED
#define PIN_BUTTON 2      // Factory reset button (INPUT_PULLUP)
#define PIN_BUZZER 4      // Buzzer
#define PIN_MOSFET 5      // MOSFET control pin (GPIO5)

/* ---------------- EEPROM layout ---------------- */

#define EEPROM_SIZE       1024
#define EEPROM_MAGIC_ADDR 0
#define EEPROM_LEN_ADDR   4
#define EEPROM_DATA_ADDR  6
const uint32_t EEPROM_MAGIC = 0xA5A55A5A;

/* ---------------- AP / WiFi / DNS ---------------- */

const char* AP_SSID = "PILAB-GPS";
const char* AP_PASS = "pilab123";

const byte DNS_PORT = 53;
DNSServer dnsServer;
ESP8266WebServer server(80);

bool hasConfig = false;
String configJson = "";

/* ---------------- Firebase ---------------- */

const String FIREBASE_BASE = "https://gps-server-neo6m-default-rtdb.firebaseio.com/";

bool oneTimeSent = false;
bool uploading = false;

/* ---------------- Blink / Button timing ---------------- */

unsigned long lastBlink = 0;
bool blinkState = false;
const unsigned long BLINK_INTERVAL = 500;

unsigned long buttonPressStart = 0;
bool buttonWasPressed = false;
const unsigned long BUTTON_HOLD_MS = 3000;

/* ---------------- GPS & RFID ---------------- */

TinyGPSPlus gps;

// RC522 pinout:
// SDA -> D8 (GPIO15), SCK -> D5 (GPIO14), MOSI -> D7 (GPIO13), MISO -> D6 (GPIO12), RST -> D3 (GPIO0)
#define SS_PIN 15   // D8
#define RST_PIN 0   // D3
MFRC522 rfid(SS_PIN, RST_PIN);

// track current RFID UID
String rfid_uid = "";

// timing for leds & buzzer on RFID
const unsigned long RFID_LED_MS = 300;   // green LED total ON time
const unsigned long RFID_BUZZ_MS = 200;  // buzzer beep time

/* ---------------- Forward declarations ---------------- */
void handleRoot();
void handleSave();
void handleNotFound();
bool saveConfigToEEPROM(const String &s);
bool loadConfigFromEEPROM();
void clearConfigEEPROM();
void setColor(bool r, bool g);
String getJsonValue(const String &json, const char *key);
String escapeJson(const String &s);
bool putToFirebase(const String &url, const String &payload);
bool getFromFirebase(const String &url, String &responseOut);
void uploadAllToFirebaseWithRetry();
void handleButton();
String escapeForJson(const String &s);
String urlEncode(const String &str);
String escapeHtml(const String &s);
bool isIpHost(const String &host);
void startAccessPointWithCaptive();
String makeSetupPage();
String trimQuotes(const String &s);

/* ======================= Basic helpers ======================= */

void setColor(bool r, bool g) {
  digitalWrite(PIN_R, r ? HIGH : LOW);
  digitalWrite(PIN_G, g ? HIGH : LOW);
}

String getJsonValue(const String &json, const char *key) {
  if (json.length() == 0) return "";
  String k = "\"" + String(key) + "\"";
  int pos = json.indexOf(k);
  if (pos < 0) return "";
  int colon = json.indexOf(':', pos);
  if (colon < 0) return "";
  int q1 = json.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = json.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return json.substring(q1 + 1, q2);
}

String escapeJson(const String &s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == '"') { o += '\\'; }
    o += c;
  }
  return o;
}

String escapeForJson(const String &s) {
  String o;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\' || c == '"') o += '\\';
    o += c;
  }
  return o;
}

String urlEncode(const String &str) {
  String encoded = "";
  char c;
  char buf[4];
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str[i];
    if ( ('0' <= c && c <= '9') ||
         ('a' <= c && c <= 'z') ||
         ('A' <= c && c <= 'Z') ||
         c == '-' || c == '_' || c == '.' || c == '~' ) {
      encoded += c;
    } else {
      sprintf(buf, "%%%02X", (uint8_t)c);
      encoded += buf;
    }
  }
  return encoded;
}

String trimQuotes(const String &s) {
  String out = s;
  out.trim();
  if (out.length() >= 2 && out[0] == '"' && out[out.length() - 1] == '"') {
    out = out.substring(1, out.length() - 1);
  }
  return out;
}

/* ======================= EEPROM config ======================= */

bool saveConfigToEEPROM(const String &s) {
  if (s.length() + EEPROM_DATA_ADDR + 4 > EEPROM_SIZE) return false;

  EEPROM.put(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  uint16_t len = s.length();
  EEPROM.put(EEPROM_LEN_ADDR, len);

  for (uint16_t i = 0; i < len; i++) EEPROM.write(EEPROM_DATA_ADDR + i, s[i]);

  EEPROM.commit();

  configJson = s;
  hasConfig = true;
  Serial.println("Config saved to EEPROM:");
  Serial.println(configJson);
  return true;
}

bool loadConfigFromEEPROM() {
  uint32_t magic;
  EEPROM.get(EEPROM_MAGIC_ADDR, magic);
  if (magic != EEPROM_MAGIC) return false;

  uint16_t len;
  EEPROM.get(EEPROM_LEN_ADDR, len);
  if (len == 0 || len > EEPROM_SIZE - EEPROM_DATA_ADDR) return false;
  if (len >= 1000) return false;

  char *buf = (char*)malloc(len + 1);
  if (!buf) return false;
  for (uint16_t i = 0; i < len; i++) buf[i] = EEPROM.read(EEPROM_DATA_ADDR + i);
  buf[len] = 0;
  configJson = String(buf);
  free(buf);
  return true;
}

void clearConfigEEPROM() {
  EEPROM.put(EEPROM_MAGIC_ADDR, 0);
  EEPROM.put(EEPROM_LEN_ADDR, 0);
  for (int i = EEPROM_DATA_ADDR; i < EEPROM_SIZE; i++) EEPROM.write(i, 0);
  EEPROM.commit();
  hasConfig = false;
  configJson = "";
  oneTimeSent = false;
  uploading = false;
  Serial.println("EEPROM cleared");
}

/* ======================= Button handling ======================= */

void handleButton() {
  int b = digitalRead(PIN_BUTTON);
  if (b == LOW) {
    if (!buttonWasPressed) {
      buttonWasPressed = true;
      buttonPressStart = millis();
    } else {
      if (millis() - buttonPressStart >= BUTTON_HOLD_MS) {
        Serial.println("Button held 3s -> Factory Reset triggered");
        clearConfigEEPROM();
        delay(300);
        ESP.restart();
      }
    }
  } else {
    buttonWasPressed = false;
  }
}

/* ======================= Firebase HTTP helpers ======================= */

bool putToFirebase(const String &url, const String &payload) {
  WiFiClientSecure client;
  client.setInsecure(); // no cert validation
  HTTPClient https;

  Serial.print("PUT -> ");
  Serial.println(url);
  Serial.print("Payload: ");
  Serial.println(payload);

  bool ok = false;
  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/json");
    int httpCode = https.PUT(payload);
    Serial.printf("HTTP code: %d\n", httpCode);
    if (httpCode >= 200 && httpCode < 300) ok = true;
    https.end();
  } else {
    Serial.println("HTTPS begin failed");
  }
  return ok;
}

bool getFromFirebase(const String &url, String &responseOut) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  Serial.print("GET -> ");
  Serial.println(url);

  if (!https.begin(client, url)) {
    Serial.println("HTTPS begin failed (GET)");
    return false;
  }

  int httpCode = https.GET();
  Serial.printf("HTTP code (GET): %d\n", httpCode);
  if (httpCode >= 200 && httpCode < 300) {
    responseOut = https.getString();
    https.end();
    Serial.print("Response: ");
    Serial.println(responseOut);
    return true;
  } else {
    https.end();
    return false;
  }
}

/* ======================= Upload config to Firebase (ONE BY ONE) ======================= */

void uploadAllToFirebaseWithRetry() {
  uploading = true;
  Serial.println("Starting NVS upload to Firebase (sending keys one by one)...");

  String veh = getJsonValue(configJson, "vehnum");
  if (veh == "") veh = "unknown";

  String rtmp1 = getJsonValue(configJson, "rtmp1");
  String rtmp2 = getJsonValue(configJson, "rtmp2");
  String rtmp3 = getJsonValue(configJson, "rtmp3");
  String rtmp4 = getJsonValue(configJson, "rtmp4");
  String imei  = getJsonValue(configJson, "imei");

  String base = FIREBASE_BASE;
  if (!base.endsWith("/")) base += "/";
  String basePath = base + "data/" + urlEncode(veh) + "/";  // e.g. .../data/Uuuuu/

  // Track which keys are uploaded successfully
  bool done_rtmp1 = false;
  bool done_rtmp2 = false;
  bool done_rtmp3 = false;
  bool done_rtmp4 = false;
  bool done_imei  = false;
  bool done_vehnum = false;
  bool done_ts    = false;

  while (true) {
    // ensure WiFi
    if (WiFi.status() != WL_CONNECTED) {
      String ssid = getJsonValue(configJson, "wifi_ssid");
      String pass = getJsonValue(configJson, "wifi_pass");
      if (ssid.length() > 0) {
        WiFi.disconnect();
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        unsigned long t0 = millis();
        while (millis() - t0 < 8000 && WiFi.status() != WL_CONNECTED) {
          server.handleClient();
          handleButton();
          delay(200);
        }
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      // rtmp1
      if (!done_rtmp1) {
        String url = basePath + "rtmp1.json";
        String payload = "\"" + escapeForJson(rtmp1) + "\"";
        done_rtmp1 = putToFirebase(url, payload);
      }

      // rtmp2
      if (!done_rtmp2) {
        String url = basePath + "rtmp2.json";
        String payload = "\"" + escapeForJson(rtmp2) + "\"";
        done_rtmp2 = putToFirebase(url, payload);
      }

      // rtmp3
      if (!done_rtmp3) {
        String url = basePath + "rtmp3.json";
        String payload = "\"" + escapeForJson(rtmp3) + "\"";
        done_rtmp3 = putToFirebase(url, payload);
      }

      // rtmp4
      if (!done_rtmp4) {
        String url = basePath + "rtmp4.json";
        String payload = "\"" + escapeForJson(rtmp4) + "\"";
        done_rtmp4 = putToFirebase(url, payload);
      }

      // imei
      if (!done_imei) {
        String url = basePath + "imei.json";
        String payload = "\"" + escapeForJson(imei) + "\"";
        done_imei = putToFirebase(url, payload);
      }

      // vehnum
      if (!done_vehnum) {
        String url = basePath + "vehnum.json";
        String payload = "\"" + escapeForJson(veh) + "\"";
        done_vehnum = putToFirebase(url, payload);
      }

      // timestampz (fresh every retry)
      if (!done_ts) {
        String url = basePath + "timestampz.json";
        String payload = "\"" + escapeForJson(String((unsigned long)millis())) + "\"";
        done_ts = putToFirebase(url, payload);
      }

      if (done_rtmp1 && done_rtmp2 && done_rtmp3 && done_rtmp4 &&
          done_imei && done_vehnum && done_ts) {
        Serial.println("All NVS keys uploaded successfully (one by one).");
        oneTimeSent = true;
        uploading = false;
        return;
      } else {
        Serial.println("Some NVS keys failed, retrying remaining ones...");
      }
    } else {
      Serial.println("WiFi not connected for NVS upload -> retrying connect");
    }

    // small backoff
    unsigned long waitStart = millis();
    while (millis() - waitStart < 3000) {
      server.handleClient();
      handleButton();
      delay(100);
    }
  }
}

/* ======================= Captive Portal / UI helpers ======================= */

String escapeHtml(const String &s) {
  String r = s;
  r.replace("&", "&amp;");
  r.replace("<", "&lt;");
  r.replace(">", "&gt;");
  r.replace("\"", "&quot;");
  r.replace("'", "&#39;");
  return r;
}

bool isIpHost(const String &host) {
  IPAddress ip;
  return ip.fromString(host);
}

String makeSetupPage() {
  int n = WiFi.scanNetworks();

  String selectedWifi = escapeHtml(getJsonValue(configJson, "wifi_ssid"));
  String wifiItems = "";

  if (n <= 0) {
    wifiItems = "<div class='wifi-empty'>No Wi-Fi networks found</div>";
  } else {
    for (int i = 0; i < n; ++i) {
      String ss = WiFi.SSID(i);
      ss = escapeHtml(ss);
      int32_t rssi = WiFi.RSSI(i);

      int bars = 1;
      if (rssi >= -60)      bars = 4;
      else if (rssi >= -70) bars = 3;
      else if (rssi >= -80) bars = 2;
      else                  bars = 1;

      String activeClass = (ss == selectedWifi) ? " wifi-item-active" : "";
      wifiItems += "<div class='wifi-item" + activeClass + "' data-ssid='" + ss + "'>";
      wifiItems +=   "<div class='wifi-name'>" + ss + "</div>";
      wifiItems +=   "<div class='wifi-signal sig-" + String(bars) + "'>";
      wifiItems +=     "<span></span><span></span><span></span><span></span>";
      wifiItems +=   "</div>";
      wifiItems += "</div>";
    }
  }

  String veh      = escapeHtml(getJsonValue(configJson, "vehnum"));
  String imei     = escapeHtml(getJsonValue(configJson, "imei"));
  String rtmp1    = escapeHtml(getJsonValue(configJson, "rtmp1"));
  String rtmp2    = escapeHtml(getJsonValue(configJson, "rtmp2"));
  String rtmp3    = escapeHtml(getJsonValue(configJson, "rtmp3"));
  String rtmp4    = escapeHtml(getJsonValue(configJson, "rtmp4"));
  String wifiPass = escapeHtml(getJsonValue(configJson, "wifi_pass"));

  String page =
    "<!doctype html><html lang='en'><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>PILAB GPS - Device Setup</title>"
    "<link href='https://fonts.googleapis.com/css2?family=Poppins:wght@300;400;600&display=swap' rel='stylesheet'>"
    "<style>"
      ":root{--ios-bg:#f2f2f7;--card-bg:#ffffff;--border:#d1d1d6;--label:#3a3a3c;"
      "--primary:#34c759;--input-bg:#ffffff;--placeholder:#9a9aa1;--toast-bg:#111827;}"
      "*{box-sizing:border-box;margin:0;padding:0;}"
      "body{font-family:'Poppins',-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
        "background:var(--ios-bg);color:#111827;}"
      ".wrap{display:flex;justify-content:center;padding:24px;min-height:100vh;}"
      ".card{background:var(--card-bg);border-radius:22px;padding:22px;max-width:520px;width:100%;"
        "box-shadow:0 18px 40px rgba(0,0,0,0.10);}"
      "h1{text-align:center;margin-bottom:8px;font-size:20px;font-weight:600;color:#111827;}"
      "label{display:block;margin-top:12px;font-weight:500;color:var(--label);font-size:13px;}"
      "input,textarea{width:100%;padding:11px 13px;margin-top:6px;border-radius:12px;border:1px solid var(--border);"
        "background:var(--input-bg);font-size:14px;}"
      "input::placeholder,textarea::placeholder{color:var(--placeholder);}"
      "input:focus,textarea:focus{border-color:var(--primary);outline:none;box-shadow:0 0 0 2px rgba(52,199,89,0.25);}"
      ".btn{width:100%;padding:14px;margin-top:22px;border:none;border-radius:16px;background:var(--primary);"
        "color:#fff;font-weight:600;font-size:15px;cursor:pointer;}"
      ".btn:active{transform:scale(0.98);}"
      "@media(max-width:520px){.card{padding:18px;border-radius:20px;}}"
      ".wifi-dropdown{position:relative;margin-top:6px;}"
      ".wifi-input{width:100%;padding:11px 13px;border-radius:12px;border:1px solid var(--border);"
        "background:var(--input-bg);padding-right:32px;font-size:14px;cursor:pointer;}"
      ".wifi-input::placeholder{color:var(--placeholder);}"
      ".wifi-input:focus{border-color:var(--primary);outline:none;box-shadow:0 0 0 2px rgba(52,199,89,0.25);}"
      ".wifi-dropdown::after{content:'\\25BE';position:absolute;right:12px;top:50%;transform:translateY(-50%);"
        "font-size:12px;color:#8e8e93;pointer-events:none;}"
      ".wifi-panel{position:absolute;left:0;right:0;top:100%;background:#fff;border-radius:16px;"
        "box-shadow:0 16px 36px rgba(0,0,0,0.16);max-height:260px;overflow-y:auto;margin-top:6px;"
        "display:none;z-index:20;border:1px solid #e5e5ea;}"
      ".wifi-panel.show{display:block;}"
      ".wifi-empty{padding:14px;font-size:13px;color:#8e8e93;text-align:center;}"
      ".wifi-item{padding:10px 14px;font-size:14px;border-bottom:1px solid #f2f2f7;cursor:pointer;"
        "display:flex;align-items:center;justify-content:space-between;}"
      ".wifi-item:last-child{border-bottom:none;}"
      ".wifi-item:hover{background:#f7f7fb;}"
      ".wifi-item-active{background:#e5f9ec;}"
      ".wifi-name{flex:1;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;padding-right:10px;}"
      ".wifi-signal{display:inline-block;width:20px;height:14px;position:relative;}"
      ".wifi-signal span{position:absolute;bottom:0;width:3px;border-radius:999px;background:#d1d1d6;}"
      ".wifi-signal span:nth-child(1){left:1px;height:4px;}"
      ".wifi-signal span:nth-child(2){left:6px;height:7px;}"
      ".wifi-signal span:nth-child(3){left:11px;height:10px;}"
      ".wifi-signal span:nth-child(4){left:16px;height:13px;}"
      ".sig-1 span:nth-child(1),"
      ".sig-2 span:nth-child(-n+2),"
      ".sig-3 span:nth-child(-n+3),"
      ".sig-4 span:nth-child(-n+4){background:#34c759;}"
      ".overlay{position:fixed;inset:0;background:rgba(0,0,0,0.25);display:flex;align-items:center;"
        "justify-content:center;z-index:50;}"
      ".overlay.hidden{display:none;}"
      ".loader-box{background:#fff;padding:18px 22px;border-radius:18px;display:flex;flex-direction:column;"
        "align-items:center;gap:10px;box-shadow:0 14px 32px rgba(0,0,0,0.24);}"
      ".spinner{width:26px;height:26px;border-radius:50%;border:3px solid #e5e5ea;border-top-color:#34c759;"
        "animation:spin 0.8s linear infinite;}"
      ".loader-text{font-size:14px;color:#111827;}"
      "@keyframes spin{to{transform:rotate(360deg);}}"
      ".toast{position:fixed;left:50%;bottom:26px;transform:translateX(-50%);background:var(--toast-bg);"
        "color:#fff;padding:10px 16px;border-radius:18px;font-size:13px;opacity:0;pointer-events:none;"
        "transition:opacity 0.25s ease-out;z-index:60;}"
      ".toast.show{opacity:1;}"
    "</style></head><body>"
    "<div class='wrap'><div class='card'>"
    "<h1>PILAB GPS DEVICE SETUP</h1>"

    "<form id='cfgForm' method='POST' action='/save'>"
      "<label>Vehicle Number *</label>"
      "<input type='text' name='veh' placeholder='GJ-01-1234' value='" + veh + "'>"

      "<label>RTMP URL 1 *</label>"
      "<input type='text' name='rtmp1' placeholder='rtmp://server/live/stream1' value='" + rtmp1 + "'>"

      "<label>RTMP URL 2 *</label>"
      "<input type='text' name='rtmp2' placeholder='rtmp://server/live/stream2' value='" + rtmp2 + "'>"

      "<label>RTMP URL 3</label>"
      "<input type='text' name='rtmp3' placeholder='Optional' value='" + rtmp3 + "'>"

      "<label>RTMP URL 4</label>"
      "<input type='text' name='rtmp4' placeholder='Optional' value='" + rtmp4 + "'>"

      "<label>IMEI Number *</label>"
      "<input type='text' name='imei' placeholder='IMEI123456789' value='" + imei + "'>"

      "<label>Select WiFi *</label>"
      "<div id='wifi_dropdown' class='wifi-dropdown'>"
        "<input id='wifi_input' class='wifi-input' type='text' "
           "placeholder='-- Select WiFi --' value='" + selectedWifi + "' readonly>"
        "<input id='wifi_ssid' type='hidden' name='wifi_ssid' value='" + selectedWifi + "'>"
        "<div id='wifi_panel' class='wifi-panel'>" + wifiItems + "</div>"
      "</div>"

      "<label>WiFi Password *</label>"
      "<input type='password' name='wifi_pass' placeholder='WiFi Password' value='" + wifiPass + "'>"

      "<button class='btn' type='submit'>Save & Restart Device</button>"
    "</form>"

    "</div></div>"

    "<div id='saving_overlay' class='overlay hidden'>"
      "<div class='loader-box'>"
        "<div class='spinner'></div>"
        "<div class='loader-text'>Saving configuration...</div>"
      "</div>"
    "</div>"

    "<div id='toast' class='toast'>Saved. Device restarting...</div>"

    "<script>"
      "document.addEventListener('DOMContentLoaded',function(){"
        "var input=document.getElementById('wifi_input');"
        "var hidden=document.getElementById('wifi_ssid');"
        "var panel=document.getElementById('wifi_panel');"
        "if(input){"
          "input.addEventListener('click',function(e){"
            "e.stopPropagation();"
            "panel.classList.toggle('show');"
          "});"
        "}"
        "panel.querySelectorAll('.wifi-item').forEach(function(el){"
          "el.addEventListener('click',function(){"
            "var ssid=this.getAttribute('data-ssid');"
            "input.value=ssid;"
            "hidden.value=ssid;"
            "panel.classList.remove('show');"
          "});"
        "});"
        "document.addEventListener('click',function(){"
          "panel.classList.remove('show');"
        "});"

        "var form=document.getElementById('cfgForm');"
        "var overlay=document.getElementById('saving_overlay');"
        "var toast=document.getElementById('toast');"
        "form.addEventListener('submit',function(e){"
          "e.preventDefault();"
          "overlay.classList.remove('hidden');"
          "var data=new FormData(form);"
          "fetch('/save',{method:'POST',body:data})"
            ".then(function(r){return r.text();})"
            ".then(function(){"
              "overlay.classList.add('hidden');"
              "toast.textContent='Saved. Device restarting...';"
              "toast.classList.add('show');"
              "setTimeout(function(){toast.classList.remove('show');},2500);"
            "})"
            ".catch(function(){"
              "overlay.classList.add('hidden');"
              "toast.textContent='Failed to save. Please try again.';"
              "toast.classList.add('show');"
              "setTimeout(function(){toast.classList.remove('show');},3000);"
            "});"
        "});"
      "});"
    "</script>"

    "</body></html>";

  return page;
}

void startAccessPointWithCaptive() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("[AP] AP started. IP = ");
  Serial.println(apIP);

  dnsServer.start(DNS_PORT, "*", apIP);  // all domains -> our IP
  Serial.println("[DNS] Captive portal DNS started");
}

/* ======================= HTTP handlers ======================= */

void handleRoot() {
  if (WiFi.getMode() & WIFI_AP) {
    String host = server.hostHeader();
    IPAddress apIP = WiFi.softAPIP();
    if (!isIpHost(host) && host != apIP.toString()) {
      String url = "http://" + apIP.toString();
      server.sendHeader("Location", url, true);
      server.send(302, "text/plain", "");
      return;
    }
  }

  String page = makeSetupPage();
  server.send(200, "text/html", page);
}

void handleNotFound() {
  handleRoot();
}

void handleSave() {
  Serial.println("[WEB] /save called");
  if (!server.hasArg("veh") || server.arg("veh") == "" ||
      !server.hasArg("imei") || server.arg("imei") == "" ||
      !server.hasArg("rtmp1") || server.arg("rtmp1") == "" ||
      !server.hasArg("rtmp2") || server.arg("rtmp2") == "" ||
      !server.hasArg("wifi_ssid") || server.arg("wifi_ssid") == "" ||
      !server.hasArg("wifi_pass") || server.arg("wifi_pass") == "") {
    Serial.println("[WEB] Missing required fields!");
    server.send(400, "application/json", "{\"status\":\"error\",\"msg\":\"Missing fields\"}");
    return;
  }

  String j = "{";
  j += "\"vehnum\":\""   + escapeJson(server.arg("veh"))       + "\",";
  j += "\"imei\":\""     + escapeJson(server.arg("imei"))      + "\",";
  j += "\"rtmp1\":\""    + escapeJson(server.arg("rtmp1"))     + "\",";
  j += "\"rtmp2\":\""    + escapeJson(server.arg("rtmp2"))     + "\",";
  j += "\"rtmp3\":\""    + escapeJson(server.arg("rtmp3"))     + "\",";
  j += "\"rtmp4\":\""    + escapeJson(server.arg("rtmp4"))     + "\",";
  j += "\"wifi_ssid\":\""+ escapeJson(server.arg("wifi_ssid")) + "\",";
  j += "\"wifi_pass\":\""+ escapeJson(server.arg("wifi_pass")) + "\"";
  j += "}";

  bool ok = saveConfigToEEPROM(j);
  if (ok) {
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    Serial.println("[WEB] Config saved. Restarting...");
    delay(1300);
    ESP.restart();
  } else {
    server.send(500, "application/json", "{\"status\":\"error\",\"msg\":\"save failed\"}");
  }
}

/* ======================= setup() ======================= */

void setup() {
  Serial.begin(9600);
  delay(100);

  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_MOSFET, OUTPUT);

  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_MOSFET, LOW);   // MOSFET OFF at start

  setColor(false, false); // all off

  EEPROM.begin(EEPROM_SIZE);
  delay(10);

  // Initialize SPI & RFID
  SPI.begin();
  rfid.PCD_Init();

  hasConfig = loadConfigFromEEPROM();

  if (!hasConfig) {
    Serial.println("No config in EEPROM -> AP setup mode (captive portal)");
    setColor(true, false); // RED solid = no config
    startAccessPointWithCaptive();
  } else {
    Serial.println("Config loaded:");
    Serial.println(configJson);
    setColor(false, true); // GREEN solid = config OK
    WiFi.mode(WIFI_STA);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

/* ======================= loop() ======================= */

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  handleButton();

  // one-time config upload
  if (hasConfig && !oneTimeSent && !uploading) {
    String ssid = getJsonValue(configJson, "wifi_ssid");
    String pass = getJsonValue(configJson, "wifi_pass");

    if (ssid.length() > 0) {
      Serial.printf("Trying to connect to WiFi SSID: %s\n", ssid.c_str());
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid.c_str(), pass.c_str());

      unsigned long t0 = millis();
      const unsigned long CONNECT_TIMEOUT = 15000;
      while (millis() - t0 < CONNECT_TIMEOUT && WiFi.status() != WL_CONNECTED) {
        server.handleClient();
        handleButton();
        delay(200);
      }

      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi connected. IP: ");
        Serial.println(WiFi.localIP());
      } else {
        Serial.println("WiFi not connected yet. Will still attempt upload routine (it will try reconnect).");
      }
    } else {
      Serial.println("No wifi_ssid in config -> cannot connect to router.");
    }

    uploadAllToFirebaseWithRetry();
  }

  if (hasConfig && oneTimeSent) {
    unsigned long now = millis();
    if (now - lastBlink >= BLINK_INTERVAL) {
      lastBlink = now;
      blinkState = !blinkState;
      if (blinkState) setColor(false, true);
      else setColor(false, false);
    }

    // GPS feed
    while (Serial.available() > 0) {
      gps.encode(Serial.read());
    }

    bool gpsValid = gps.location.isValid();
    if (!gpsValid) {
      setColor(true, true); // yellow = NO FIX
    } else {
      if (blinkState) setColor(false, true);
      else setColor(false, false);
    }

    // ===== RFID logic (single JSON + feedback + MOSFET control) =====
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String new_uid = "";
      for (byte i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) new_uid += "0";
        new_uid += String(rfid.uid.uidByte[i], HEX);
        if (i < rfid.uid.size - 1) new_uid += " ";
      }
      new_uid.toUpperCase();

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();

      rfid_uid = new_uid;

      Serial.print("RFID detected: ");
      Serial.println(rfid_uid);

      // ---- Immediate feedback: card read (short) ----
      setColor(false, true);          // green ON
      digitalWrite(PIN_BUZZER, HIGH);
      unsigned long tstart = millis();
      while (millis() - tstart < 100) {   // 100 ms quick beep
        server.handleClient();
        handleButton();
        delay(5);
      }
      digitalWrite(PIN_BUZZER, LOW);
      setColor(false, false);             // off before match

      int matchedIndex = 0;

      if (WiFi.status() == WL_CONNECTED) {
        String veh = getJsonValue(configJson, "vehnum");
        if (veh == "") veh = "unknown";

        String base = FIREBASE_BASE;
        if (!base.endsWith("/")) base += "/";

        // GET full rfid_data JSON once
        String urlRF = base + "data/" + urlEncode(veh) + "/rfid_data.json";
        String rfidJson;
        if (getFromFirebase(urlRF, rfidJson)) {
          if (rfidJson != "null") {
            String uid1 = trimQuotes(getJsonValue(rfidJson, "uid1"));
            String uid2 = trimQuotes(getJsonValue(rfidJson, "uid2"));
            String uid3 = trimQuotes(getJsonValue(rfidJson, "uid3"));
            String uid4 = trimQuotes(getJsonValue(rfidJson, "uid4"));

            uid1.trim(); uid2.trim(); uid3.trim(); uid4.trim();
            uid1.toUpperCase(); uid2.toUpperCase(); uid3.toUpperCase(); uid4.toUpperCase();

            if (uid1.length() > 0 && uid1 == rfid_uid) matchedIndex = 1;
            else if (uid2.length() > 0 && uid2 == rfid_uid) matchedIndex = 2;
            else if (uid3.length() > 0 && uid3 == rfid_uid) matchedIndex = 3;
            else if (uid4.length() > 0 && uid4 == rfid_uid) matchedIndex = 4;

            if (matchedIndex > 0) {
              Serial.print("RFID match found at index: ");
              Serial.println(matchedIndex);

              // ---- MOSFET control ----
              if (matchedIndex == 1) {
                digitalWrite(PIN_MOSFET, HIGH);  // ON
                Serial.println("MOSFET ON (uid1 match)");
              } else if (matchedIndex == 2) {
                digitalWrite(PIN_MOSFET, LOW);   // OFF
                Serial.println("MOSFET OFF (uid2 match)");
              }

              // ---- SUCCESS feedback: green + longer beep ----
              setColor(false, true);      // green ON
              digitalWrite(PIN_BUZZER, HIGH);
              unsigned long tstart2 = millis();
              while (millis() - tstart2 < RFID_BUZZ_MS) {   // 200 ms
                server.handleClient();
                handleButton();
                delay(5);
              }
              digitalWrite(PIN_BUZZER, LOW);

              unsigned long t2 = millis();
              while (millis() - t2 < (RFID_LED_MS - RFID_BUZZ_MS)) { // rest of 300 ms
                server.handleClient();
                handleButton();
                delay(5);
              }
              setColor(false, false);

              // Build ONE JSON with uid1..4 + current + status
              String payloadRF = "{";
              payloadRF += "\"uid1\":\"" + escapeForJson(uid1) + "\",";
              payloadRF += "\"uid2\":\"" + escapeForJson(uid2) + "\",";
              payloadRF += "\"uid3\":\"" + escapeForJson(uid3) + "\",";
              payloadRF += "\"uid4\":\"" + escapeForJson(uid4) + "\",";
              payloadRF += "\"current\":\"" + escapeForJson(rfid_uid) + "\",";
              payloadRF += "\"status\":\"" + String(matchedIndex) + "\"";
              payloadRF += "}";

              // Single PUT to rfid_data.json
              putToFirebase(urlRF, payloadRF);
            } else {
              Serial.println("RFID not found in uid1..uid4.");
            }
          } else {
            Serial.println("rfid_data is null in Firebase.");
          }
        } else {
          Serial.println("Failed to GET rfid_data.json from Firebase.");
        }
      } else {
        Serial.println("WiFi not connected -> cannot verify RFID against Firebase.");
      }

      // Restore LED according to GPS/blink state
      if (!gpsValid) setColor(true, true);          // yellow (no fix)
      else if (blinkState) setColor(false, true);   // green blink
      else setColor(false, false);                  // off
    }
    // ===== END RFID logic =====

    // Build location JSON
    String lat = gps.location.isValid() ? String(gps.location.lat(), 6) : String("");
    String lon = gps.location.isValid() ? String(gps.location.lng(), 6) : String("");
    String alt = gps.altitude.isValid() ? String(gps.altitude.meters()) : String("");
    String speed = gps.speed.isValid() ? String(gps.speed.kmph()) : String("");
    String sat = gps.satellites.isValid() ? String(gps.satellites.value()) : String("");
    String timeStr = gps.time.isValid() ? String(gps.time.hour()) + ":" + String(gps.time.minute()) + ":" + String(gps.time.second()) : String("");
    String dateStr = gps.date.isValid() ? String(gps.date.day()) + "-" + String(gps.date.month()) + "-" + String(gps.date.year()) : String("");

    String uidToSend = rfid_uid.length() ? rfid_uid : String("");

    String locPayload = "{";
    locPayload += "\"UID\":\"" + escapeForJson(uidToSend) + "\",";
    locPayload += "\"lat\":\"" + escapeForJson(lat) + "\",";
    locPayload += "\"lon\":\"" + escapeForJson(lon) + "\",";
    locPayload += "\"alt\":\"" + escapeForJson(alt) + "\",";
    locPayload += "\"speed\":\"" + escapeForJson(speed) + "\",";
    locPayload += "\"sat\":\"" + escapeForJson(sat) + "\",";
    locPayload += "\"time\":\"" + escapeForJson(timeStr) + "\",";
    locPayload += "\"date\":\"" + escapeForJson(dateStr) + "\"";
    locPayload += "}";

    if (WiFi.status() != WL_CONNECTED) {
      String ssid = getJsonValue(configJson, "wifi_ssid");
      String pass = getJsonValue(configJson, "wifi_pass");
      if (ssid.length() > 0) {
        WiFi.begin(ssid.c_str(), pass.c_str());
        unsigned long t0 = millis();
        while (millis() - t0 < 8000 && WiFi.status() != WL_CONNECTED) {
          server.handleClient();
          handleButton();
          delay(200);
        }
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      String veh = getJsonValue(configJson, "vehnum");
      if (veh == "") veh = "unknown";
      String base = FIREBASE_BASE;
      if (!base.endsWith("/")) base += "/";
      String url = base + "data/" + urlEncode(veh) + "/location.json";
      bool ok = putToFirebase(url, locPayload);
      if (!ok) {
        Serial.println("Location PUT failed (will retry next loop)");
      }
    }

    delay(10);
  }
}
