// BUILD_TIMESTAMP: 2026-04-23 23:18:12
// FILE_BUILD_TIMESTAMP: 2026-04-23 12:51:18
// net.cpp — Полностью автономный файл с исправленным AP-режимом + SET + RESET + CONFIG
// Модель LLM: Flash 2.5 (Gemini)
// Build: 2026-01-15 14:11 CET
// Оптимизация RAM: HTML в PROGMEM (экономия ~3.3 КБ)

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "config.h"
#include "HX711.h"
#include <user_interface.h>

// ===============================================================
//                    HTML TEMPLATES (PROGMEM)
// ===============================================================

const char HTML_MAIN[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<style>
body{background:#111;color:#fff;text-align:center;font-family:Arial;margin:0}
.btn{width:80%;padding:18px;margin:12px auto;display:block;background:#222;color:#fff;font-size:28px;border-radius:14px;border:2px solid #444;text-decoration:none}
.btn:hover{background:#444}
h1{font-size:38px;padding:18px}
</style></head><body>
<h1>%s RECOVERY</h1>
<a class='btn' href='/update'>Firmware Update</a>
<a class='btn' href='/flash'>Download Data (BIN)</a>
<a class='btn' href='/report'>Download Report (TXT)</a>
<a class='btn' href='/monitor'>Live Monitoring</a>
<a class='btn' href='/hx'>HX711 Tare</a>
<a class='btn' href='/set'>SET</a>
<a class='btn' href='/exit'>Exit & Restart</a>
</body></html>
)rawliteral";

const char HTML_SET[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name='viewport' content='width=device-width,initial-scale=1'/>
<style>
body{background:#111;color:#fff;font-family:Arial;text-align:center;margin:0;padding:0}
label{font-size:26px;display:block;text-align:left;width:90%%;margin:8px auto 4px}
input{width:90%%;font-size:28px;padding:8px;margin:4px auto 14px;border-radius:10px;border:2px solid #444;background:#000;color:#fff}
.btn{width:90%%;padding:22px;font-size:30px;margin:16px auto;display:block;background:#222;color:#fff;border:2px solid #555;border-radius:12px;text-decoration:none}
</style></head><body>
<h2 style='font-size:34px;'>SETTINGS</h2>
<form method='POST' action='/save'>
<label>Device Name:</label>
<input name='devname' value='%s' placeholder='ESP8266_SOLAR'><br>
<label>WiFi Password:</label>
<input name='pass' value='%s' placeholder='Password'><br>
<label>WiFi AP #1:</label>
<input name='wifi1' value='%s' placeholder='SSID 1'><br>
<label>WiFi AP #2:</label>
<input name='wifi2' value='%s' placeholder='SSID 2'><br>
<label>WiFi AP #3:</label>
<input name='wifi3' value='%s' placeholder='SSID 3'><br>
<label>WiFi AP #4:</label>
<input name='wifi4' value='%s' placeholder='SSID 4'><br>
<label>SMS Number:</label>
<input name='sms' value='%s' placeholder='+7XXXXXXXXXX'><br>
<label>CALL Number:</label>
<input name='call' value='%s' placeholder='+7XXXXXXXXXX'><br>
<label>Hourly Sleep (sec):</label>
<input name='sleep' value='%u' placeholder='3600'><br>
<label>Voltage Divider K:</label>
<input name='voltK' value='%.3f' placeholder='1.92'><br>
<label>GSM Module:</label>
<input type='checkbox' name='gsm' value='1' %s><br>
<button class='btn' type='submit'>💾 SAVE</button>
</form>
<a class='btn' style='background:#550' href='/reset_data'>Reset DATA</a>
<a class='btn' style='background:#850' href='/reset_config'>Reset CONFIG</a>
<a class='btn' style='background:#a00' href='/full_wipe'>FULL WIPE</a>
<a class='btn' href='/exit'>Exit</a>
<a class='btn' href='/'>Back</a>
</body></html>
)rawliteral";

const char HTML_MONITOR[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
body{background:#111;color:#0f0;font-family:monospace;padding:20px}
pre{font-size:26px;line-height:1.4;background:#000;padding:15px;border-radius:10px}
.btn{display:block;width:80%;margin:30px auto;padding:20px;background:#333;color:#fff;font-size:36px;border-radius:15px;text-decoration:none}
</style></head><body>
<h1 style="color:#0f0">LIVE MONITOR</h1>
<pre id="d">Loading...</pre>
<a class="btn" href="/">Menu</a>
<script>
setInterval(()=>{fetch('/monitor_data').then(r=>r.text()).then(t=>document.getElementById('d').textContent=t)},4000);
</script>
</body></html>
)rawliteral";

// ===============================================================
//                    ГЛОБАЛЬНЫЕ ОБЪЕКТЫ (ВНЕШНИЕ)
// ===============================================================
extern ESP8266WebServer HttpServer;
extern ESP8266HTTPUpdateServer httpUpdater;
extern void measureAndSaveHour();
extern void handleCalibrate();
extern void handleCalibrate2();
extern String buildDailyReportText();

extern SoftwareSerial sim800; 

// ===============================================================
//                    ВНЕШНИЕ ФУНКЦИИ ИЗ date.cpp
// ===============================================================
#include "rtc.h"
extern bool loadConfig();
extern void saveConfig();

// ===============================================================
//                    AP MODE — ГЛАВНАЯ СТРАНИЦА
// ===============================================================
String buildMainPage() {
    String name = cfg.deviceName.isEmpty() ? "ESP8266" : cfg.deviceName;
    char html[1024];
    // Исправляем формат для PROGMEM
    snprintf_P(html, sizeof(html), HTML_MAIN, name.c_str());
    return String(html);
}
// ===============================================================
//                    СТРАНИЦА SET
// ===============================================================
void handleSet() {
    loadConfig();
    
    char html[2560];
    snprintf(html, sizeof(html), HTML_SET,
        cfg.deviceName.c_str(),
        cfg.pass.c_str(),
        cfg.wifi1.c_str(),
        cfg.wifi2.c_str(),
        cfg.wifi3.c_str(),
        cfg.wifi4.c_str(),
        cfg.sms.c_str(),
        cfg.call.c_str(),
        cfg.sleepSec,
        cfg.voltK,
        cfg.gsmEnabled ? "checked" : ""
    );
    
    HttpServer.send(200, "text/html", html);
}

void handleSave() {
    if (!LittleFS.begin()) {
        HttpServer.send(200, "text/plain", "LittleFS init failed");
        return;
    }

    if (HttpServer.hasArg("devname")) cfg.deviceName = HttpServer.arg("devname");
    if (HttpServer.hasArg("pass"))    cfg.pass       = HttpServer.arg("pass");
    if (HttpServer.hasArg("wifi1"))   cfg.wifi1      = HttpServer.arg("wifi1");
    if (HttpServer.hasArg("wifi2"))   cfg.wifi2      = HttpServer.arg("wifi2");
    if (HttpServer.hasArg("wifi3"))   cfg.wifi3      = HttpServer.arg("wifi3");
    if (HttpServer.hasArg("wifi4"))   cfg.wifi4      = HttpServer.arg("wifi4");
    if (HttpServer.hasArg("sms"))     cfg.sms        = HttpServer.arg("sms");
    if (HttpServer.hasArg("call"))    cfg.call       = HttpServer.arg("call");
    if (HttpServer.hasArg("sleep"))   cfg.sleepSec   = HttpServer.arg("sleep").toInt();
    if (HttpServer.hasArg("voltK"))   cfg.voltK      = HttpServer.arg("voltK").toFloat();
    
    cfg.gsmEnabled = HttpServer.hasArg("gsm");

    saveConfig();

    HttpServer.send(200, "text/plain", "CONFIG SAVED\nRestarting...");
    delay(50);
    ESP.restart();
}

// ===============================================================
//                    RESET HANDLERS
// ===============================================================
void handleResetData() {
    if (!LittleFS.begin()) {
        HttpServer.send(200, "text/plain", "LittleFS init failed");
        return;
    }

    LittleFS.remove("/day.bin");
    LittleFS.remove("/report.txt");

    HttpServer.send(200, "text/plain", "DATA removed (/day.bin, /report.txt)\nRestarting...");
    delay(50);
    ESP.restart();
}

void handleResetConfig() {
    if (!LittleFS.begin()) {
        HttpServer.send(200, "text/plain", "LittleFS init failed");
        return;
    }

    LittleFS.remove("/config.json");

    HttpServer.send(200, "text/plain", "CONFIG removed (/config.json)\nRestarting...");
    delay(50);
    ESP.restart();
}

void handleFullWipe() {
    HttpServer.send(200, "text/plain", "FULL WIPE LittleFS\nRestarting...");
    delay(30);

    if (LittleFS.begin()) {
        LittleFS.format();
    }

    delay(50);
    ESP.restart();
}

// ===============================================================
//                    HX711 TARE PAGE + ACTIONS
// ===============================================================
void handleHxPage() {
    String s = F("<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>body{background:#111;color:#fff;font-family:Arial;text-align:center}"
        ".btn{width:90%;padding:18px;margin:12px auto;display:block;background:#222;"
        "color:#fff;font-size:28px;border-radius:14px;border:2px solid #444;text-decoration:none}"
        ".btn:hover{background:#444}</style></head><body>");
    s += F("<h2>HX711 Tare</h2>");
    if (hasHx711Tare()) {
        s += "<p>Saved offset: <b>" + String(hx711TareOffset()) + "</b></p>";
    } else {
        s += F("<p><b>No saved tare</b></p>");
    }
    s += F("<a class='btn' href='/hx_tare'>TARE (save to flash)</a>");
    s += F("<a class='btn' style='background:#850' href='/hx_clear'>Clear saved tare</a>");
    s += F("<a class='btn' href='/'>&#8592; Back</a>");
    s += F("</body></html>");
    HttpServer.send(200, "text/html", s);
}

static long doHx711TareAndGetOffset() {
    if (cfg.gsmEnabled) sim800.end();
    delay(10);

    pinMode(SOL_GSM_RX, OUTPUT);
    digitalWrite(SOL_GSM_RX, LOW);
    pinMode(ACC_GSM_TX, INPUT_PULLUP);
    delay(50);

    HX711 hxLocal;
    hxLocal.begin(ACC_GSM_TX, SOL_GSM_RX); // DT, SCK
    hxLocal.set_scale(1.0f);
    hxLocal.tare(20);
    long off = hxLocal.get_offset();
    hxLocal.power_down();

    pinMode(SOL_GSM_RX, INPUT);
    pinMode(ACC_GSM_TX, INPUT_PULLUP);
    if (cfg.gsmEnabled) {
        sim800.begin(9600);
        delay(8);
    }
    return off;
}

void handleHxTare() {
    long off = doHx711TareAndGetOffset();
    bool ok = saveHx711Tare(off);
    HttpServer.send(200, "text/plain",
        ok ? "HX711 TARE saved. Restarting..." : "Failed to save HX711 tare. Restarting...");
    delay(50);
    ESP.restart();
}

void handleHxClear() {
    clearHx711Tare();
    HttpServer.send(200, "text/plain", "HX711 tare cleared. Restarting...");
    delay(50);
    ESP.restart();
}

// ===============================================================
//                    FLASH / REPORT / MONITOR
// ===============================================================
void handleFlash() {
    if (!LittleFS.begin() || !LittleFS.exists("/day.bin")) {
        HttpServer.send(200, "text/plain", "NO DATA");
        return;
    }
    File f = LittleFS.open("/day.bin", "r");
    HttpServer.streamFile(f, "application/octet-stream");
    f.close();
}

void handleReport() {
    if (!LittleFS.begin()) {
        HttpServer.send(200, "text/plain", "FS ERROR");
        return;
    }

    if (!LittleFS.exists("/report.txt")) {
        if (DEBUG_SIM800) Serial.printf("[AP] Building report on demand...\n");
        File f2 = LittleFS.open("/report.txt", "w");
        if (f2) {
            f2.print(buildDailyReportText());
            f2.close();
        }
    }

    File f = LittleFS.open("/report.txt", "r");
    HttpServer.streamFile(f, "text/plain; charset=utf-8");
    f.close();
}

void handleMonitor() {
    HttpServer.send_P(200, PSTR("text/html"), HTML_MONITOR);
}
void handleMonitorData() {
    measureAndSaveHour();

    String dev = cfg.deviceName;
    if (dev.isEmpty()) dev = "ESP8266";

    String s = "=== LIVE DATA: " + dev + " ===\n";
    s += "Build: " + String(OTA_VERSION) + "\n";
    s += "Uptime: " + String(millis()/1000) + "s\n\n";

    HourRecord& h = rtcHour(rtc.wakeHour);
    
    // Конвертируем из новых форматов
    float t_ds = (h.t_ds5 == -127) ? -127 : h.t_ds5 / 2.0f;
    float t_bme = (h.t_bme5 == -127) ? -127 : h.t_bme5 / 2.0f;
    float hum = (float)h.hum5;
    float press = 900.0f + h.pressDelta;
    float weight = h.weight2 / 2.0f;
    float acc = h.acc50 / 50.0f;
    float sol = h.solar50 / 50.0f;
    float chg = h.charge50 / 50.0f;
    
    s += "T_DS:    " + String(t_ds, 1) + " °C\n";
    s += "T_BME:   " + String(t_bme, 1) + " °C\n";
    s += "Hum:     " + String(hum, 0) + " %\n";
    s += "Press:   " + String(press, 0) + " hPa\n";
    s += "PIR:     " + String(digitalRead(PIR_AP_GND_SENS)) + "\n";  // Читаем PIR напрямую
    s += "Acc:     " + String(acc, 3) + "V\n";
    s += "Solar:   " + String(sol, 3) + "V\n";
    s += "Charge:  " + String(chg, 3) + "V\n";
    s += "Weight:  " + String(weight, 2) + " kg\n";

    HttpServer.send(200, "text/plain", s);
}
// ===============================================================
//              WiFi Connect с использованием RTC memory
// ===============================================================
bool wifi_connect(String& outBestSSID, int& outBestRSSI, int& outBestChannel, 
                  uint8_t outBestBSSID[6], String& scanLog) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    Serial.println("[WiFi] Starting scan...");
    
    // Полное сканирование сетей
    int n = WiFi.scanNetworks(false, true);
    if (n <= 0) {
        Serial.printf("[WiFi] Scan failed or no networks found: %d\n", n);
        scanLog = "\n[SCAN] No networks found\n\n";
        return false;
    }

    Serial.printf("[WiFi] Found %d networks\n", n);

    // Сортировка для лога
    struct Net { String ssid; int32_t rssi; uint8_t enc; uint8_t bssid[6]; int channel; };
    Net nets[20];
    int count = min(n, 20);
    
    for (int i = 0; i < count; i++) {
        nets[i].ssid = WiFi.SSID(i);
        nets[i].rssi = WiFi.RSSI(i);
        nets[i].enc = WiFi.encryptionType(i);
        nets[i].channel = WiFi.channel(i);
        memcpy(nets[i].bssid, WiFi.BSSID(i), 6);
        
        // Отладочный вывод каждой найденной сети
        Serial.printf("[WiFi] Found: %s (CH%d, RSSI:%d, %s)\n", 
                      nets[i].ssid.c_str(), 
                      nets[i].channel, 
                      nets[i].rssi,
                      nets[i].enc == ENC_TYPE_NONE ? "OPEN" : "LOCKED");
    }
    
    // Сортировка по RSSI (пузырьковая)
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (nets[j].rssi > nets[i].rssi) {
                Net temp = nets[i];
                nets[i] = nets[j];
                nets[j] = temp;
            }
        }
    }

    // Формируем лог
    scanLog = "\n[WiFi Scan (sorted)]:\n";
    for (int i = 0; i < count; i++) {
        scanLog += String(i+1) + ". " + nets[i].ssid + " (CH" + String(nets[i].channel) + ") " + 
                   String(nets[i].rssi) + " dBm" +
                   (nets[i].enc == ENC_TYPE_NONE ? " [OPEN]" : " [LOCK]") + "\n";
        
        // Дополнительный отладочный вывод в Serial
        Serial.printf("[WiFi] Sorted %d: %s (CH%d, %d dBm)\n", 
                      i+1, nets[i].ssid.c_str(), nets[i].channel, nets[i].rssi);
    }
    scanLog += "\n";

    // Поиск лучшей известной сети
    outBestRSSI = -999;
    String known[4] = {cfg.wifi1, cfg.wifi2, cfg.wifi3, cfg.wifi4};
    
    Serial.println("[WiFi] Looking for known networks:");
    for (int w = 0; w < 4; w++) {
        if (!known[w].isEmpty()) {
            Serial.printf("  Known SSID %d: '%s'\n", w+1, known[w].c_str());
        }
    }
    
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        
        for (int w = 0; w < 4; w++) {
            if (!known[w].isEmpty() && ssid == known[w]) {
                Serial.printf("[WiFi] Found known network: %s (RSSI: %d)\n", ssid.c_str(), rssi);
                if (rssi > outBestRSSI) {
                    outBestSSID = ssid;
                    outBestRSSI = rssi;
                    memcpy(outBestBSSID, WiFi.BSSID(i), 6);
                    outBestChannel = WiFi.channel(i);
                    Serial.printf("[WiFi] Selected as best: %s (CH%d, RSSI:%d)\n", 
                                  outBestSSID.c_str(), outBestChannel, outBestRSSI);
                }
            }
        }
    }

    if (outBestRSSI == -999) {
        Serial.println("[WiFi] No known networks found in scan results");
        return false;
    }

    Serial.printf("[WiFi] Attempting to connect to %s (Channel %d, RSSI %d)\n", 
                  outBestSSID.c_str(), outBestChannel, outBestRSSI);

    // Подключение
    WiFi.begin(outBestSSID.c_str(), cfg.pass.c_str(), 
               outBestChannel, outBestBSSID, true);
    
    uint32_t t0 = millis();
    uint32_t timeout = 15000; // 15 секунд таймаут
    Serial.print("[WiFi] Connecting ");
    
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeout) {
        delay(500);
        Serial.print(".");
        // Выводим статус каждые 2 секунды
        if ((millis() - t0) % 2000 < 500) {
            Serial.printf(" (status: %d)", WiFi.status());
        }
        yield();
    }
    Serial.println();

    // Если подключились успешно - сохраняем параметры в RTC
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] ✅ Connected successfully, IP: %s\n", 
                      WiFi.localIP().toString().c_str());
        Serial.printf("[WiFi] RSSI: %d dBm, Channel: %d\n", WiFi.RSSI(), WiFi.channel());
        
        memcpy(rtc.wifiBssid, outBestBSSID, 6);
        rtc.wifiChannel = outBestChannel;
        rtcSave();
        return true;
    } else {
        Serial.printf("[WiFi] ❌ Connection failed, status: %d\n", WiFi.status());
        
        // Выводим возможные причины
        switch(WiFi.status()) {
            case WL_NO_SSID_AVAIL:
                Serial.println("[WiFi] Network not found");
                break;
            case WL_CONNECT_FAILED:
                Serial.println("[WiFi] Connection failed - wrong password?");
                break;
            case WL_WRONG_PASSWORD:
                Serial.println("[WiFi] Wrong password");
                break;
            case WL_DISCONNECTED:
                Serial.println("[WiFi] Disconnected during connection attempt");
                break;
            default:
                Serial.printf("[WiFi] Unknown status code: %d\n", WiFi.status());
        }
        return false;
    }
}
// ===============================================================
//                 ENTER AP MODE
// ===============================================================
void enterAPMode(uint32_t durationSec) {
    if (DEBUG_SIM800) Serial.printf("[AP] Entering AP mode for %u seconds\n", durationSec);

    loadConfig();

    WiFi.mode(WIFI_AP);
    String apName = cfg.deviceName;
    if (apName.isEmpty()) apName = "ESP_RECOVERY";
    apName += "_" + String(ESP.getChipId(), HEX);
    WiFi.softAP(apName.c_str(), "");

    // Сохраняем данные при входе в AP
    if (LittleFS.begin()) {
        File f = LittleFS.open("/day.bin", "w");
        if (f) {
            f.write((uint8_t*)rtc.hours, sizeof(rtc.hours));
            f.close();
            if (DEBUG_SIM800) Serial.printf("[AP] /day.bin saved (240 bytes)\n");
        }
    }

    bool cfgOk = loadConfig();
    if (!cfgOk) {
        saveConfig();
    }

    HttpServer.on("/", [](){ HttpServer.send(200, "text/html", buildMainPage()); });
    HttpServer.on("/flash", handleFlash);
    HttpServer.on("/report", handleReport);
    HttpServer.on("/monitor", handleMonitor);
    HttpServer.on("/monitor_data", handleMonitorData);

    HttpServer.on("/set", handleSet);
    HttpServer.on("/save", HTTP_POST, handleSave);
    HttpServer.on("/reset_data", handleResetData);
    HttpServer.on("/reset_config", handleResetConfig);
    HttpServer.on("/full_wipe", handleFullWipe);
    HttpServer.on("/calibrate", handleCalibrate);
    HttpServer.on("/calibrate2", HTTP_POST, handleCalibrate2);
    HttpServer.on("/hx", handleHxPage);
    HttpServer.on("/hx_tare", handleHxTare);
    HttpServer.on("/hx_clear", handleHxClear);

    HttpServer.on("/exit", [](){
        HttpServer.send(200,"text/plain","Restarting...");
        delay(30);
        ESP.restart();
    });

    httpUpdater.setup(&HttpServer, "/update");

    HttpServer.begin();
    if (DEBUG_SIM800) Serial.printf("[AP] Server started\n");

    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    static unsigned long ledMs = 0;
    static bool ledOn = false;

    uint32_t end = millis() + durationSec * 1000UL;
    while (millis() < end) {
        HttpServer.handleClient();
        yield();

        unsigned long now = millis();
        if (ledOn) {
            if (now - ledMs >= 10) {
                digitalWrite(LED_BUILTIN, HIGH);
                ledOn = false;
                ledMs = now;
            }
        } else {
            if (now - ledMs >= 1000) {
                digitalWrite(LED_BUILTIN, LOW);
                ledOn = true;
                ledMs = now;
            }
        }
    }

    if (DEBUG_SIM800) Serial.printf("[AP] Timeout → Restart\n");
    ESP.restart();
}

// ===============================================================
//                    GSM ФУНКЦИИ
// ===============================================================
String sim800Command(const String& cmd, uint32_t timeout = 1000) {
    sim800.println(cmd);
    String resp = "";
    unsigned long t = millis();
    while (millis() - t < timeout) {
        while (sim800.available()) {
            resp += (char)sim800.read();
        }
        if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) break;
        yield();
    }
    resp.trim();
    if (DEBUG_SIM800) Serial.printf("[SIM800] CMD:'%s' RESP:'%s'\n", cmd.c_str(), resp.c_str());
    return resp;
}
// ===============================================================
//                ПРОВЕРКА НАЛИЧИЯ SIM800
// ===============================================================
bool checkSim800Presence() {
    Serial.printf("[SIM800] Checking presence (timeout 10s)...\n");
    
    // Включаем питание SIM800
    digitalWrite(GSM_POW_Ring_Zamok, HIGH);
    delay(100);
    
    // Настраиваем пины для SIM800
    pinMode(ACC_GSM_TX, OUTPUT);
    pinMode(SOL_GSM_RX, INPUT);
    
    sim800.begin(9600);
    delay(50);
    
    // Очищаем буфер
    while (sim800.available()) sim800.read();
    
    // Отправляем AT и ждём ответ
    unsigned long start = millis();
    bool detected = false;
    String response = "";
    
    while (millis() - start < 10000) {
        sim800.println("AT");
        
        unsigned long cmdStart = millis();
        while (millis() - cmdStart < 500) {
            while (sim800.available()) {
                char c = sim800.read();
                response += c;
                if (response.indexOf("OK") != -1) {
                    detected = true;
                    break;
                }
            }
            if (detected) break;
            delay(10);
        }
        
        if (detected) {
            Serial.printf("[SIM800] DETECTED after %lu ms\n", millis() - start);
            digitalWrite(GSM_POW_Ring_Zamok, LOW);
            break;
        }
        
        response = "";
        delay(200);
    }
    
    if (!detected) {
        Serial.printf("[SIM800] NOT DETECTED after 10 seconds\n");
        digitalWrite(GSM_POW_Ring_Zamok, LOW);
    }
    
    return detected;
}
bool sim800Init() {
    sim800.end();
    delay(5);
    pinMode(ACC_GSM_TX, OUTPUT);
    pinMode(SOL_GSM_RX, INPUT);
    sim800.begin(9600);
    delay(10); // SIM800 иногда не успевает сразу, можно увеличить до 20 мс по желанию
    
    static bool     simReady = false;
    static uint32_t lastInitMs = 0;

    if (simReady && (millis() - lastInitMs) < 180000UL) {
        return true;
    }

    if (simReady) {
        String pong = sim800Command("AT", 300);
        if (pong.indexOf("OK") != -1) {
            lastInitMs = millis();
            return true;
        }
    }

    if (DEBUG_SIM800) Serial.printf("[GSM] Initializing SIM800...\n");

    pinMode(ACC_GSM_TX, OUTPUT);
    pinMode(SOL_GSM_RX, INPUT);

    String resp = sim800Command("AT", 1500);
    if (resp.indexOf("OK") == -1) {
        if (DEBUG_SIM800) Serial.printf("[GSM] No response to AT\n");
        simReady = false;
        return false;
    }

    sim800Command("ATE0", 1000);
    sim800Command("AT+CPIN?", 1000);
    
    // === ВКЛЮЧЕНИЕ АОН (Caller ID) ===
    String clipResp = sim800Command("AT+CLIP=1", 1000);
    if (clipResp.indexOf("OK") != -1) {
        if (DEBUG_SIM800) Serial.printf("[GSM] AON (CLIP) enabled successfully\n");
    } else {
        if (DEBUG_SIM800) Serial.printf("[GSM] Failed to enable AON (CLIP): %s\n", clipResp.c_str());
    }
    // =================================

    if (DEBUG_SIM800) Serial.printf("[GSM] Waiting for network registration...\n");
    unsigned long start = millis();
    unsigned long lastLog = 0;
    bool registered = false;

    while (millis() - start < 15000) {
        String creg = sim800Command("AT+CREG?", 1000);

        if (creg.indexOf(",1") > 0 || creg.indexOf(",5") > 0) {
            if (DEBUG_SIM800) Serial.printf("[GSM] Registered! (%s)\n", creg.c_str());
            registered = true;
            break;
        }

        if (millis() - lastLog > 2000) {
            if (DEBUG_SIM800) Serial.printf("[GSM] Not registered yet... (%s)\n", creg.c_str());
            lastLog = millis();
        }
        yield();
    }

    if (!registered) {
        if (DEBUG_SIM800) Serial.printf("[GSM] REGISTRATION TIMEOUT — continuing anyway\n");
    }

    String csq = sim800Command("AT+CSQ", 1000);
    if (DEBUG_SIM800) Serial.printf("[GSM] Signal: %s\n", csq.c_str());

    if (registered) {
        if (DEBUG_SIM800) Serial.printf("[GSM] Waiting for network time sync...\n");
        
        unsigned long timeStart = millis();
        bool timeValid = false;
        
        while (millis() - timeStart < 25000) {
            String cclk = sim800Command("AT+CCLK?", 1000);
            
            int q1 = cclk.indexOf('"');
            if (q1 != -1 && (int)cclk.length() > q1 + 3) {
                String yearStr = cclk.substring(q1 + 1, q1 + 3);
                int year = yearStr.toInt();
                
                if (year >= 24) {
                    if (DEBUG_SIM800) Serial.printf("[GSM] Time synced: %s\n", cclk.c_str());
                    timeValid = true;
                    break;
                }
            }
            
            if ((millis() - timeStart) % 5000 < 200) {
                if (DEBUG_SIM800) Serial.printf("[GSM] Waiting for time... (%lus)\n", 
                                                 (millis() - timeStart) / 1000);
            }
            
            delay(100);
            yield();
        }
        
        if (!timeValid) {
            if (DEBUG_SIM800) Serial.printf("[GSM] WARNING: Time NOT synced (using 2004 default)\n");
        }
    }

    if (DEBUG_SIM800) Serial.printf("[GSM] Ready\n");

    simReady = true;
    lastInitMs = millis();
    return true;
}
int sim800GetRSSI() {
    String r = sim800Command("AT+CSQ");
    if (r.startsWith("+CSQ:")) {
        int i = r.indexOf(":") + 2;
        int j = r.indexOf(",", i);
        return r.substring(i, j).toInt();
    }
    return -1;
}

bool sim800GetBattery(int& p, int& mv) {
    String r = sim800Command("AT+CBC");
    if (r.startsWith("+CBC:")) {
        int i = r.indexOf(",") + 1;
        int j = r.indexOf(",", i);
        p = r.substring(i, j).toInt();
        mv = r.substring(j + 1).toInt();
        return true;
    }
    return false;
}

bool sim800GetClock(String& t, String& d) {
    String r = sim800Command("AT+CCLK?");
    if (r.indexOf("+CCLK:") != -1) {
        int s = r.indexOf('"') + 1;
        int e = r.indexOf('"', s);
        if (s == 0 || e == -1 || e <= s) return false;
        String dt = r.substring(s, e);

        int comma = dt.indexOf(',');
        if (comma == -1) return false;

        d = dt.substring(0, comma);

        String timePart = dt.substring(comma + 1);

        int signPos = timePart.indexOf('+');
        if (signPos == -1) signPos = timePart.indexOf('-');

        String timeOnly = (signPos == -1) ? timePart : timePart.substring(0, signPos);

        if (timeOnly.length() < 8) {
            while (timeOnly.length() < 8) timeOnly = "0" + timeOnly;
        }

        t = timeOnly;
        return true;
    }
    return false;
}
bool sim800SendSMS(const String& phone, const String& msg) {
    // Убрали end(), begin() и delay'и
    pinMode(ACC_GSM_TX, OUTPUT);
    pinMode(SOL_GSM_RX, INPUT);
    delay(10);
    
    // Очистка буфера
    while (sim800.available()) sim800.read();

    // ============================================
    // ОТЛАДОЧНЫЙ ВЫВОД СОДЕРЖИМОГО SMS
    // ============================================
    #if DEBUG_SIM800
        Serial.printf("\n[SMS_DEBUG] =========================================\n");
        Serial.printf("[SMS_DEBUG] Отправка SMS на номер: %s\n", phone.c_str());
        Serial.printf("[SMS_DEBUG] Длина сообщения: %d символов\n", msg.length());
        Serial.printf("[SMS_DEBUG] Содержимое SMS:\n");
        Serial.printf("[SMS_DEBUG] %s\n", msg.c_str());
        Serial.printf("[SMS_DEBUG] =========================================\n\n");
    #endif

    sim800Command("AT+CMGF=1", 800);
    sim800Command("AT+CSCS=\"GSM\"", 800);

    String cmd = String("AT+CMGS=\"") + phone + String("\"");
    sim800.println(cmd);

    unsigned long t = millis();
    bool prompt = false;
    String partial = "";
    while (millis() - t < 10000) {  // увеличил таймаут до 10 сек
        while (sim800.available()) {
            char c = sim800.read();
            partial += c;
            if (c == '>') {
                prompt = true;
                break;
            }
        }
        if (prompt) break;
        yield();
    }

    if (!prompt) {
        Serial.printf("[GSM] NO '>' prompt after CMGS! Response so far: %s\n", partial.c_str());
        return false;
    }
    
    #if DEBUG_SIM800
        Serial.printf("[GSM] Got '>' prompt, sending message (%d chars)...\n", msg.length());
    #endif

    sim800.print(msg);
    sim800.write(0x1A);

    unsigned long t2 = millis();
    String resp = "";
    while (millis() - t2 < 20000) {
        while (sim800.available()) {
            resp += (char)sim800.read();
        }
        if (resp.indexOf("OK") != -1) {
            #if DEBUG_SIM800
                Serial.printf("[GSM] SMS sent OK\n");
            #endif
            return true;
        }
        if (resp.indexOf("ERROR") != -1) {
            #if DEBUG_SIM800
                Serial.printf("[GSM] SMS send ERROR resp: %s\n", resp.c_str());
            #endif
            return false;
        }
        yield();
    }

    #if DEBUG_SIM800
        Serial.printf("[GSM] SMS send TIMEOUT, resp so far: %s\n", resp.c_str());
    #endif
    return false;
}
void sim800Debug() {
    // Добавляем проверку наличия SIM800
    if (!isSim800Present()) {
        Serial.printf("[SIM] SIM800 not present, debug skipped\n");
        return;
    }
    
#if DEBUG_SIM800
    Serial.printf("\n[SIM] ----- FULL GSM DEBUG START -----\n");
    sim800.end();
    delay(5);
    pinMode(ACC_GSM_TX, OUTPUT);
    pinMode(SOL_GSM_RX, INPUT);
    sim800.begin(9600);
    delay(10);
    
    auto dbg = [&](const String& cmd, uint16_t to = 1500) {
        Serial.printf("CMD: %s\n", cmd.c_str());
        String r = sim800Command(cmd, to);
        Serial.printf("RESP: %s\n\n", r.c_str());
    };

    dbg("AT");
    dbg("ATE0");
    dbg("AT+CPIN?");
    dbg("AT+CREG?");
    dbg("AT+CSQ");
    dbg("AT+COPS?");
    dbg("AT+CBAND?");
    dbg("AT+CGATT?");
    dbg("AT+CBC");
    dbg("AT+CCLK?");
    dbg("AT+GSN");

    Serial.printf("[SIM] ----- FULL GSM DEBUG END -----\n\n");
#endif
}

void ALARM() {
    if (DEBUG_SIM800) Serial.printf("[ALARM] PIR_AP_GND_SENS held LOW > 2s → ALARM TRIGGERED!\n");
    loadConfig();

    if (cfg.gsmEnabled && isSim800Present()) {
        sim800Init();
        sim800SendSMS(cfg.sms, "ALARM: PIR ALERT!");
    } else if (cfg.gsmEnabled && !isSim800Present()) {
        if (DEBUG_SIM800) Serial.printf("[ALARM] SIM800 not present according to RTC flag → SMS not sent\n");
    } else {
        if (DEBUG_SIM800) Serial.printf("[ALARM] GSM disabled in config → SMS not sent\n");
    }

    delay(20);
    ESP.restart();
}