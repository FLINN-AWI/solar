// BUILD_TIMESTAMP: 2026-04-23 23:18:12
// FILE_BUILD_TIMESTAMP: 2026-04-23 23:18:12
// date.cpp — Перенос основной логики из main.cpp
// Модель LLM: Flash 2.5 (Gemini)
// Build: 2025-12-06 15:28 CET

#include <Arduino.h>
#include <LittleFS.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <WiFiClientSecure.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ESP8266WiFiMulti.h>
#include <EspSleep.h>
#include <AutoOTA.h>
#include <FastBot2.h>
#include <SoftwareSerial.h>
#include <time.h>
#include <user_interface.h>
#include "config.h"
#include "rtc.h" 
#include "HX711.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_BME280.h>
#include <Wire.h>

AutoOTA ota(OTA_VERSION, OTA_PATH);
// Добавьте в начало date.cpp после всех include:
extern ConfigData cfg;

// ===============================================================
//                    ФУНКЦИИ РАБОТЫ С КОНФИГОМ
// ===============================================================
bool loadConfig() {
    if (!LittleFS.begin() || !LittleFS.exists("/config.json")) return false;
    File f = LittleFS.open("/config.json", "r");
    if (!f) return false;
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    
    cfg.pass = doc["pass"] | DEFAULT_PASS;
    cfg.wifi1 = doc["wifi1"] | DEFAULT_WIFI_AP_1;
    cfg.wifi2 = doc["wifi2"] | DEFAULT_WIFI_AP_2;
    cfg.wifi3 = doc["wifi3"] | "";
    cfg.wifi4 = doc["wifi4"] | "";
    cfg.sms = doc["sms"] | SMS_NUMBER;
    cfg.call = doc["call"] | CALL_NUMBER1;
    cfg.sleepSec = doc["sleep"] | SLEEP_SEC;
    cfg.deviceName = doc["devname"] | DEFAULT_DEVICE_NAME;
    cfg.gsmEnabled = doc["gsm"] | DEFAULT_GSM_ENABLED;
    cfg.voltK = doc["voltK"] | DEFAULT_VOLT_K;
    cfg.scaleK = doc["scaleK"] | DEFAULT_SCALE_K;
    return true;
}

void saveConfig() {
    if (!LittleFS.begin()) return;
    
    JsonDocument doc;
    doc["pass"] = cfg.pass;
    doc["wifi1"] = cfg.wifi1;
    doc["wifi2"] = cfg.wifi2;
    doc["wifi3"] = cfg.wifi3;
    doc["wifi4"] = cfg.wifi4;
    doc["sms"] = cfg.sms;
    doc["call"] = cfg.call;
    doc["sleep"] = cfg.sleepSec;
    doc["devname"] = cfg.deviceName;
    doc["gsm"] = cfg.gsmEnabled ? 1 : 0;
    doc["voltK"] = cfg.voltK;
    doc["scaleK"] = cfg.scaleK;
    
    File f = LittleFS.open("/config.json", "w");
    if (f) {
        serializeJson(doc, f);
        f.close();
    }
}
extern ESP8266WebServer HttpServer;
extern ESP8266HTTPUpdateServer httpUpdater;
extern SoftwareSerial sim800;
extern ConfigData cfg;
extern String wifiScanBeforeConnect;
extern void measureAndSaveHour();
extern bool sim800Init();
extern int sim800GetRSSI();
extern bool sim800GetBattery(int& percent, int& voltageMv);
extern bool sim800GetClock(String& TimeSIM, String& DateSIM);
extern bool sim800SendSMS(const String& phone, const String& msg);
extern String sim800Command(const String& cmd, uint32_t timeout);
extern void ALARM();
extern void enterAPMode(uint32_t durationSec);
extern void sim800Debug();

bool wifi_connect(String& outBestSSID, int& outBestRSSI, int& outBestChannel, uint8_t outBestBSSID[6], String& scanLog);

extern FastBot2 bot;
extern FastBot2 bot2;
// ===============================================================
//                 TIME SANITY + SIM800 -> EPOCH (UTC)
// ===============================================================
static bool isSaneEpoch(uint32_t ts) {
    // 1700000000 ~ 2023-11, 2200000000 ~ 2039-09 (32-bit safe window)
    return (ts >= 1700000000UL && ts <= 2200000000UL);
}

// Minimal UTC epoch builder (good for years 1970..2099+)
static uint32_t epochFromUTC(int year, int mon, int mday, int hour, int min, int sec) {
    // year: full (e.g. 2026), mon: 1..12
    static const int daysBeforeMonth[12] =
        {0,31,59,90,120,151,181,212,243,273,304,334};

    auto isLeap = [](int y) {
        return ((y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0));
    };

    long days = 0;
    for (int y = 1970; y < year; y++) days += isLeap(y) ? 366 : 365;

    days += daysBeforeMonth[mon - 1];
    if (mon > 2 && isLeap(year)) days += 1;
    days += (mday - 1);

    long total = days * 86400L + hour * 3600L + min * 60L + sec;
    if (total < 0) total = 0;
    return (uint32_t)total;
}

// Read SIM800 time with timezone, convert to UTC epoch.
// Returns true only if epoch passes sanity check.
static bool getEpochFromSim800(uint32_t &outEpoch) {
    outEpoch = 0;

    if (!cfg.gsmEnabled || !isSim800Present()) return false;
    if (!sim800Init()) return false;

    String r = sim800Command("AT+CCLK?", 1200);
    int q1 = r.indexOf('"');
    if (q1 == -1) return false;
    int q2 = r.indexOf('"', q1 + 1);
    if (q2 == -1) return false;

    // Example: 26/04/06,11:39:49+12  (TZ in quarters of an hour)
    String dt = r.substring(q1 + 1, q2);
    int comma = dt.indexOf(',');
    if (comma == -1) return false;

    String d = dt.substring(0, comma);     // "YY/MM/DD"
    String t = dt.substring(comma + 1);    // "HH:MM:SS+TZ" or "HH:MM:SS-TZ"

    if (d.length() < 8 || t.length() < 8) return false;

    int yy = d.substring(0, 2).toInt();
    int mm = d.substring(3, 5).toInt();
    int dd = d.substring(6, 8).toInt();

    int hh = t.substring(0, 2).toInt();
    int mi = t.substring(3, 5).toInt();
    int ss = t.substring(6, 8).toInt();

    // TZ is in quarters of an hour per GSM 07.07
    int tzQuarters = 0;
    int signPos = t.indexOf('+');
    int sign = +1;
    if (signPos == -1) { signPos = t.indexOf('-'); sign = -1; }
    if (signPos != -1 && (int)t.length() >= signPos + 3) {
        tzQuarters = t.substring(signPos + 1, signPos + 3).toInt() * sign;
    }

    // Reject SIM800 default like "04/01/01"
    if (yy < 24 || mm < 1 || mm > 12 || dd < 1 || dd > 31 ||
        hh > 23 || mi > 59 || ss > 59) {
        return false;
    }

    int year = 2000 + yy;
    uint32_t localEpoch = epochFromUTC(year, mm, dd, hh, mi, ss);

    long tzSeconds = (long)tzQuarters * 15L * 60L;
    long utcEpoch = (long)localEpoch - tzSeconds;
    if (utcEpoch < 0) utcEpoch = 0;

    outEpoch = (uint32_t)utcEpoch;
    return isSaneEpoch(outEpoch);
}
static uint16_t g_lastSim800BattMv = 0; // обновляется в sendDailyReport() (AT+CBC)
static int16_t g_lastSim800CSQ = -1; // 0..31, 99; -1 = unknown

// ===============================================================
//                ИЗМЕРЕНИЕ ВЕСА
// ===============================================================

// ===============================================================
//                    HX711 PERSISTENT TARE (LittleFS)
// ===============================================================

static const char* HX_TARE_FILE = "/hx711.json";
static const char* HX_TARE_LOCK = "/hx711.lock";

static bool g_hasHxTare = false;
static long g_hxOffset = 0;
/*
bool hasHx711Tare() { return g_hasHxTare; }
long hx711TareOffset() { return g_hxOffset; }
static bool createHx711TareLock() {
    if (!LittleFS.begin()) return false;

    if (LittleFS.exists(HX_TARE_LOCK)) return true;

    File f = LittleFS.open(HX_TARE_LOCK, "w");
    if (!f) return false;

    f.println("LOCK");
    f.close();

    Serial.println("[HX711] ✅ Tare LOCK created (/hx711.lock)");
    return true;
}

bool loadHx711Tare() {
    g_hasHxTare = false;
    g_hxOffset = 0;
    if (!LittleFS.begin()) return false;
    if (!LittleFS.exists(HX_TARE_FILE)) return false;
    File f = LittleFS.open(HX_TARE_FILE, "r");
    if (!f) return false;
    JsonDocument doc;
    auto err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    if (!doc["offset"].is<long>()) return false;
    g_hxOffset = doc["offset"].as<long>();
    g_hasHxTare = true;
    Serial.printf("[HX711] Loaded tare offset from flash: %ld\n", g_hxOffset);
    return true;
}
bool saveHx711Tare(long offset) {
    if (!LittleFS.begin()) return false;
    JsonDocument doc;
    doc["offset"] = offset;
    File f = LittleFS.open(HX_TARE_FILE, "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    g_hxOffset = offset;
    g_hasHxTare = true;
    Serial.printf("[HX711] Tare offset saved to flash: %ld\n", offset);
    return true;
}
void clearHx711Tare() {
    if (LittleFS.begin()) LittleFS.remove(HX_TARE_FILE);   // вместо "/hx711.json"
    g_hasHxTare = false;
    g_hxOffset = 0;
    Serial.println("[HX711] Tare offset cleared from flash");
}
*/




HX711 hx;
static float lastWeight = 0.0f;
static bool  weightBaselineReady = false;   // NEW: чтобы 1-е чтение не прилипало к 0

// ===============================================================
//                    ВОССТАНОВЛЕНИЕ ПИНОВ
// ===============================================================
void restoreSim800Pins() {
    hx.power_down();
    
    pinMode(SOL_GSM_RX, INPUT);           // SOL_GSM_RX
    pinMode(ACC_GSM_TX,  INPUT_PULLUP);    // ACC_GSM_TX
    
    if (cfg.gsmEnabled) {
        sim800.begin(9600);
        delay(8);
    }
    delay(5);
}

/*
// ===============================================================
//                    HX711 PERSISTENT TARE (LittleFS)
// ===============================================================

// NEW: tare files

// Делает tare и сохраняет offset в LittleFS.
// Возвращает true если успешно.
bool ensureHx711TareSaved() {
    // 1) Если уже загружено в RAM — ок
    if (hasHx711Tare()) {
        Serial.printf("[HX711] Tare already loaded: %ld\n", hx711TareOffset());
        return true;
    }

    // 2) Проверяем FS состояние
    if (!LittleFS.begin()) {
        Serial.println("[HX711] ERROR: LittleFS.begin() failed");
        return false;
    }

    bool hasJson = LittleFS.exists(HX_TARE_FILE);
    bool hasLock = LittleFS.exists(HX_TARE_LOCK);

    // 2a) Нормальный случай: есть json -> просто загрузим
    if (hasJson) {
        bool ok = loadHx711Tare();
        Serial.println(ok ? "[HX711] ✅ Tare loaded from /hx711.json"
                          : "[HX711] ❌ Failed to load /hx711.json");
        return ok;
    }

    // 2b) ВАЖНОЕ: lock есть, а json нет -> авто-tare запрещён
    if (hasLock && !hasJson) {
        Serial.println("[HX711] ❌ Tare LOCK exists but /hx711.json is missing.");
        Serial.println("[HX711] ❌ Auto-tare is DISABLED to avoid taring under hive load.");
        Serial.println("[HX711] Action: restore /hx711.json from backup or do manual tare via /hx page.");
        return false;
    }

    // 2c) Самый первый запуск: нет json и нет lock -> можно делать auto-tare
    Serial.println("[HX711] First-time tare (no json, no lock) -> performing tare and saving...");

    if (cfg.gsmEnabled) sim800.end();
    delay(10);

    pinMode(SOL_GSM_RX, OUTPUT);
    digitalWrite(SOL_GSM_RX, LOW);
    pinMode(ACC_GSM_TX, INPUT_PULLUP);
    delay(80);

    HX711 hxLocal;
    hxLocal.begin(ACC_GSM_TX, SOL_GSM_RX); // DT, SCK
    hxLocal.set_scale(1.0f);
    hxLocal.tare(25);

    long off = hxLocal.get_offset();
    hxLocal.power_down();

    restoreSim800Pins();

    if (!saveHx711Tare(off)) {
        Serial.println("[HX711] ERROR: failed to save tare to /hx711.json");
        return false;
    }

    // Сразу создаём lock
    if (!createHx711TareLock()) {
        Serial.println("[HX711] ERROR: failed to create /hx711.lock");
        // Тут можно вернуть false (строго), но json уже записан.
        // Я предлагаю вернуть true, но залогировать проблему.
        return true;
    }

    Serial.printf("[HX711] ✅ First-time tare saved and locked: %ld\n", off);
    return true;
}
//                    ПРОВЕРКА ЖЕЛЕЗА HX711
// ===============================================================
bool hx711HardwareOk(int tries = 4) {
    Serial.println("[HX711] Hardware check...");

    if (cfg.gsmEnabled) sim800.end();
    delay(5);

    pinMode(SOL_GSM_RX, OUTPUT);
    digitalWrite(SOL_GSM_RX, LOW);
    pinMode(ACC_GSM_TX, INPUT_PULLUP);
    delay(10);

hx.begin(ACC_GSM_TX, SOL_GSM_RX);   // DT, SCK HX711 out-gpio13, sck-gpio12
;
    // Negative scale: raw decreases when weight increases (inverted load cell direction)
    hx.set_scale(-cfg.scaleK);

    bool ok = false;
    for (int i = 0; i < tries; i++) {
        if (hx.wait_ready_retry(3, 150)) {
            long raw = hx.read();
            if (raw != 0) {
                ok = true;
                break;
            }
        }
        delay(100);
    }

    restoreSim800Pins();
    Serial.println(ok ? "[HX711] ✅ Hardware OK" : "[HX711] ❌ Hardware FAIL");
    return ok;
}
*/
 
 // Единственная функция управления тарой HX711.
// Политика:
// - если есть /hx711.json -> загружаем offset (в RAM)
// - если json нет, но есть /hx711.lock -> НИЧЕГО не тарим (это защита от тары под ульем)
// - если нет и json, и lock -> это первый запуск -> делаем tare, сохраняем json, создаём lock
/*
bool ensureHx711TareSaved() {
    if (g_hasHxTare) return true;

    if (!LittleFS.begin()) {
        Serial.println("[HX711] ERROR: LittleFS.begin() failed");
        return false;
    }

    const bool hasJson = LittleFS.exists(HX_TARE_FILE);
    const bool hasLock = LittleFS.exists(HX_TARE_LOCK);

    // 1) обычный режим: json существует -> просто грузим
    if (hasJson) {
        File f = LittleFS.open(HX_TARE_FILE, "r");
        if (!f) {
            Serial.println("[HX711] ERROR: open /hx711.json failed");
            return false;
        }
        JsonDocument doc;
        auto err = deserializeJson(doc, f);
        f.close();
        if (err || !doc["offset"].is<long>()) {
            Serial.println("[HX711] ERROR: parse /hx711.json failed");
            return false;
        }

        g_hxOffset = doc["offset"].as<long>();
        g_hasHxTare = true;
        Serial.printf("[HX711] Loaded tare offset: %ld\n", g_hxOffset);
        return true;
    }

    // 2) защита: lock есть, json нет -> авто-tare запрещён
    if (hasLock && !hasJson) {
        Serial.println("[HX711] ❌ LOCK exists but /hx711.json missing -> auto-tare BLOCKED");
        Serial.println("[HX711] Use /hx page to do manual tare or restore /hx711.json backup.");
        return false;
    }

    // 3) первый запуск: нет json и нет lock -> делаем tare и сохраняем
    Serial.println("[HX711] First boot: no json+no lock -> performing tare and saving...");

    // отключаем GSM serial (как у тебя)
    if (cfg.gsmEnabled) sim800.end();
    delay(10);

    // переключаем пины под HX711
    pinMode(SOL_GSM_RX, OUTPUT);
    digitalWrite(SOL_GSM_RX, LOW);
    pinMode(ACC_GSM_TX, INPUT_PULLUP);
    delay(80);

    HX711 hxLocal;
    hxLocal.begin(ACC_GSM_TX, SOL_GSM_RX);
    hxLocal.set_scale(1.0f);
    hxLocal.tare(25);

    const long off = hxLocal.get_offset();
    hxLocal.power_down();

    // вернуть пины/Serial обратно
    restoreSim800Pins();

    // сохранить json
    {
        JsonDocument doc;
        doc["offset"] = off;
        File f = LittleFS.open(HX_TARE_FILE, "w");
        if (!f) {
            Serial.println("[HX711] ERROR: write /hx711.json failed");
            return false;
        }
        serializeJson(doc, f);
        f.close();
    }

    // создать lock
    {
        File f = LittleFS.open(HX_TARE_LOCK, "w");
        if (f) { f.println("LOCK"); f.close(); }
        else {
            Serial.println("[HX711] WARNING: failed to create /hx711.lock (json saved anyway)");
        }
    }

    g_hxOffset = off;
    g_hasHxTare = true;
    Serial.printf("[HX711] ✅ First-time tare saved+locked: %ld\n", g_hxOffset);
    return true;
}
*/
// ===============================================================
//                    ОСНОВНАЯ ФУНКЦИЯ ИЗМЕРЕНИЯ ВЕСА
// ===============================================================
float readWeight() {
    // ------------------------------------------------------------
    // 1) Ensure tare offset is loaded (or saved on first boot)
    // ------------------------------------------------------------
    if (!g_hasHxTare) {
        if (!LittleFS.begin()) {
            Serial.println("[HX711] ERROR: LittleFS.begin() failed");
            return lastWeight;
        }

        const bool hasJson = LittleFS.exists(HX_TARE_FILE);
        const bool hasLock = LittleFS.exists(HX_TARE_LOCK);

        if (hasJson) {
            // load /hx711.json
            File f = LittleFS.open(HX_TARE_FILE, "r");
            if (!f) {
                Serial.println("[HX711] ERROR: open /hx711.json failed");
                return lastWeight;
            }

            JsonDocument doc;
            auto err = deserializeJson(doc, f);
            f.close();

            if (err || !doc["offset"].is<long>()) {
                Serial.println("[HX711] ERROR: parse /hx711.json failed");
                return lastWeight;
            }

            g_hxOffset = doc["offset"].as<long>();
            g_hasHxTare = true;
            Serial.printf("[HX711] Loaded tare offset from flash: %ld\n", g_hxOffset);
        }
        else if (hasLock) {
            // protection: do NOT auto-tare under hive load
            Serial.println("[HX711] ❌ /hx711.lock exists but /hx711.json missing -> auto-tare BLOCKED");
            Serial.println("[HX711] Restore /hx711.json or do manual tare via web.");
            return lastWeight;
        }
        else {
            // first boot: no json + no lock -> do tare and save
            Serial.println("[HX711] First boot: no json+no lock -> performing tare and saving...");

            if (cfg.gsmEnabled) sim800.end();
            delay(10);

            pinMode(SOL_GSM_RX, OUTPUT);
            digitalWrite(SOL_GSM_RX, LOW);
            pinMode(ACC_GSM_TX, INPUT_PULLUP);
            delay(80);

            HX711 hxLocal;
            hxLocal.begin(ACC_GSM_TX, SOL_GSM_RX);
            hxLocal.set_scale(1.0f);
            hxLocal.tare(25);

            const long off = hxLocal.get_offset();
            hxLocal.power_down();

            // restore pins/serial back (your existing helper)
            restoreSim800Pins();

            // save json
            {
                JsonDocument doc;
                doc["offset"] = off;
                File f = LittleFS.open(HX_TARE_FILE, "w");
                if (!f) {
                    Serial.println("[HX711] ERROR: write /hx711.json failed");
                    return lastWeight;
                }
                serializeJson(doc, f);
                f.close();
            }

            // create lock
            {
                File f = LittleFS.open(HX_TARE_LOCK, "w");
                if (f) { f.println("LOCK"); f.close(); }
                else Serial.println("[HX711] WARNING: failed to create /hx711.lock (json saved anyway)");
            }

            g_hxOffset = off;
            g_hasHxTare = true;
            Serial.printf("[HX711] ✅ First-time tare saved+locked: %ld\n", g_hxOffset);
        }
    }

    // ------------------------------------------------------------
    // 2) Normal weight measurement (uses g_hxOffset)
    // ------------------------------------------------------------
    const int warmup = 3;
    const int samples = 7;
    float readings[samples];

    if (cfg.gsmEnabled) sim800.end();
    delay(10);

    pinMode(SOL_GSM_RX, OUTPUT);
    digitalWrite(SOL_GSM_RX, LOW);
    pinMode(ACC_GSM_TX, INPUT_PULLUP);
    delay(50);

    hx.begin(ACC_GSM_TX, SOL_GSM_RX);

    long raw = hx.read();
    Serial.printf("[HX711] Raw ADC: %ld\n", raw);

    // IMPORTANT: you use negative scale (inverted)
    hx.set_scale(-cfg.scaleK);
    hx.set_offset(g_hxOffset);
    hx.power_up();

    delay(200);

    if (!hx.wait_ready_retry(10, 150)) {
        Serial.println("[HX711] Not ready");
        restoreSim800Pins();
        return lastWeight;
    }

    for (int i = 0; i < warmup; i++) {
        (void)hx.get_units(1);
        delay(60);
    }

    for (int i = 0; i < samples; i++) {
        readings[i] = hx.get_units(1);
        delay(60);
    }

    hx.power_down();
    restoreSim800Pins();

    std::sort(readings, readings + samples);
    float median = readings[samples / 2];
    Serial.printf("[HX711] median before clamp: %.3f\n", median);

    // baseline/hysteresis (as you had)
    if (!weightBaselineReady) {
        weightBaselineReady = true;
        lastWeight = median;
    } else {
        if (fabs(median - lastWeight) < 0.08f) median = lastWeight;
        else lastWeight = median;
    }

    Serial.printf("[HX711] Weight: %.3f kg\n", median);
    return median;
}
// ===============================================================
//                    КАЛИБРОВКА (через веб)
// ===============================================================
void handleCalibrate() {
    String html = R"rawliteral(
<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'/>
<style>body{font-family:Arial;background:#111;color:#0f0;text-align:center;padding:20px;}</style>
</head><body>
<h2>HX711 Calibration</h2>
<p><b>Automatic tare was already done on first boot.</b></p>
<p>Place known weight and press button below:</p>
<form method='POST' action='/calibrate2'>
    <input name='w' value='1.00' style='font-size:26px;width:140px'> kg<br><br>
    <button type='submit' style='font-size:28px;padding:15px 40px'>CALIBRATE SCALE</button>
</form>
<a href='/'>← Back</a>
</body></html>
)rawliteral";

    HttpServer.send(200, "text/html", html);
}

void handleCalibrate2() {
    float known = HttpServer.arg("w").toFloat();
    if (known <= 0.01f) known = 1.0f;

    if (cfg.gsmEnabled) sim800.end();
    delay(5);

    pinMode(SOL_GSM_RX, OUTPUT);
    digitalWrite(SOL_GSM_RX, LOW);
    pinMode(ACC_GSM_TX, INPUT_PULLUP);
    delay(50);

hx.begin(ACC_GSM_TX, SOL_GSM_RX);   // DT, SCK
;
    hx.set_scale(1.0f);
    hx.tare(12);
    float reading = hx.get_units(15);       // может быть отрицательным при инверсии
    cfg.scaleK = fabs(reading) / known;     // сохраняем всегда положительное
    saveConfig();

    hx.power_down();
    restoreSim800Pins();

    char msg[600];
    snprintf(msg, sizeof(msg),
        "<h2>Calibration Done!</h2>"
        "<p><b>scaleK = %.5f</b></p>"
        "<p>Known: %.3f kg | Raw: %.1f</p>"
        "<a href='/'>&#8592; Back</a>", cfg.scaleK, known, reading);

    HttpServer.send(200, "text/html", msg);
    Serial.printf("[HX711] New scaleK = %.5f\n", cfg.scaleK);
}
// ===============================================================
//     СТРУКТУРА ДЛЯ РЕЗУЛЬТАТА КАНАЛА
// ===============================================================
struct ChannelResult {
    float voltage;
    float avgADC;
    int samples;
    const char* name;
};
ChannelResult measureSingleChannel(uint8_t pin, const char* channelName) {
    ChannelResult result;
    result.name = channelName;
    result.samples = 5;
    
#if DEBUG_GLOBAL
    Serial.printf("[VOLT][%s] Measuring (5 samples)...\n", channelName);
#endif
    
    long sum = 0;
    
    digitalWrite(pin, LOW);
    delay(5);
    
    // Снять 5 сэмплов БЕЗ Serial (быстро!)
    for (int i = 0; i < 5; i++) {
        int reading = analogRead(A0);
        sum += reading;
#if DEBUG_GLOBAL
        Serial.printf("[VOLT][%s]   Sample %d: ADC = %d\n", channelName, i+1, reading);
#endif
    }
    
    digitalWrite(pin, HIGH);
    
    result.avgADC = sum / 5.0f;
    result.voltage = (result.avgADC / 1023.0) * 3.3 * cfg.voltK;
    
#if DEBUG_GLOBAL
    Serial.printf("[VOLT][%s] Average: ADC = %.1f, Voltage = %.3f V\n", 
                  channelName, result.avgADC, result.voltage);
#endif
    
    return result;
}

void measureAllChannelsOnce(ChannelResult& ch1, ChannelResult& ch2, ChannelResult& ch3) {
#if DEBUG_GLOBAL
    Serial.printf("\n[MEASURE] ==========================================\n");
    Serial.printf("[MEASURE] SEQUENTIAL 3-CHANNEL MEASUREMENT\n");
    Serial.printf("[MEASURE] Each channel: 5 samples averaged\n");
    Serial.printf("[MEASURE] Stabilization: 5 ms, Pause: 300 ms\n");
    Serial.printf("[MEASURE] ==========================================\n\n");
#endif
    
    uint32_t startTime = millis();
    
    // ИНИЦИАЛИЗАЦИЯ
    pinMode(SOL_GSM_RX, INPUT_PULLUP);
    pinMode(PIN_CHARGE,   INPUT_PULLUP);
    pinMode(ACC_GSM_TX, INPUT_PULLUP);
    delay(10);
    
    // КАНАЛ 1: ACCUM
#if DEBUG_GLOBAL
    Serial.printf("[MEASURE] ┌─── CHANNEL 1: ACCUM ───┐\n");
#endif
    
    pinMode(ACC_GSM_TX, OUTPUT);
    digitalWrite(ACC_GSM_TX, HIGH);
    delay(5);
    
    ch1 = measureSingleChannel(ACC_GSM_TX, "ACCUM");
    
    digitalWrite(ACC_GSM_TX, HIGH);
    pinMode(ACC_GSM_TX, INPUT_PULLUP);
    
#if DEBUG_GLOBAL
    Serial.printf("[MEASURE] └────────────────────────────┘\n\n");
#endif
    
    pinMode(ACC_GSM_TX, OUTPUT);
    pinMode(SOL_GSM_RX, INPUT_PULLUP);
    
#if DEBUG_GLOBAL
    Serial.printf("[MEASURE] ⏸ Waiting 300 ms (6τ)...\n\n");
#endif
    delay(300);
    
    // КАНАЛ 2: CHARGE
#if DEBUG_GLOBAL
    Serial.printf("[MEASURE] ┌─── CHANNEL 2: SOLAR ───┐\n");
#endif
    
    pinMode(PIN_CHARGE, OUTPUT);
    digitalWrite(PIN_CHARGE, HIGH);
    delay(10);
    
    ch3 = measureSingleChannel(PIN_CHARGE, "CHARGE");
    
    digitalWrite(PIN_CHARGE, HIGH);
    pinMode(PIN_CHARGE, INPUT_PULLUP);
    
#if DEBUG_GLOBAL
    Serial.printf("[MEASURE] └────────────────────────────┘\n\n");
#endif
    
    pinMode(ACC_GSM_TX, OUTPUT);
    pinMode(SOL_GSM_RX, INPUT_PULLUP);
    
#if DEBUG_GLOBAL
    Serial.printf("[MEASURE] ⏸ Waiting 300 ms (6τ)...\n\n");
#endif
    delay(300);
    
    // КАНАЛ 3: SOLAR
#if DEBUG_GLOBAL
    Serial.printf("[MEASURE] ┌─── CHANNEL 3: CHARGE ───┐\n");
#endif
    

        pinMode(SOL_GSM_RX, OUTPUT);
    digitalWrite(SOL_GSM_RX, HIGH);
    delay(5);
    
    ch2 = measureSingleChannel(SOL_GSM_RX, "SOLAR");
    
    digitalWrite(SOL_GSM_RX, HIGH);
    pinMode(SOL_GSM_RX, INPUT_PULLUP);
#if DEBUG_GLOBAL
    Serial.printf("[MEASURE] └─────────────────────────────┘\n\n");
#endif
    
    uint32_t totalTime = millis() - startTime;
    
#if DEBUG_GLOBAL
    Serial.printf("[MEASURE] ==========================================\n");
    Serial.printf("[MEASURE] ✓ ALL MEASUREMENTS COMPLETED\n");
    Serial.printf("[MEASURE] ==========================================\n");
    Serial.printf("[MEASURE] Total time: %u ms\n", totalTime);
    Serial.printf("[MEASURE] %s: %.3f V (%.1f ADC, %d samples)\n", 
                  ch1.name, ch1.voltage, ch1.avgADC, ch1.samples);
    Serial.printf("[MEASURE] %s: %.3f V (%.1f ADC, %d samples)\n", 
                  ch2.name, ch2.voltage, ch2.avgADC, ch2.samples);
    Serial.printf("[MEASURE] %s: %.3f V (%.1f ADC, %d samples)\n", 
                  ch3.name, ch3.voltage, ch3.avgADC, ch3.samples);
    Serial.printf("[MEASURE] ==========================================\n\n");
#endif
    
    pinMode(PIN_CHARGE, OUTPUT);
    digitalWrite(PIN_CHARGE, HIGH);
    delay(2);
    pinMode(ACC_GSM_TX, OUTPUT);
    pinMode(SOL_GSM_RX, INPUT_PULLUP);
}
// ===============================================================
//     ИЗМЕРЕНИЕ И СОХРАНЕНИЕ ЧАСА
// ===============================================================
void measureAndSaveHour() {
    Serial.printf("\n[MEASURE] ======================================\n");
    Serial.printf("[MEASURE] Hour %u — STARTING MEASUREMENTS\n", rtc.wakeHour);
    Serial.printf("[MEASURE] ======================================\n\n");



    // ============================================================
    //    ИЗМЕРЕНИЕ 3 КАНАЛОВ
    // ============================================================
    ChannelResult accum, solar, charge;
    measureAllChannelsOnce(accum, solar, charge);
    
    float acc = accum.voltage;
    float sol = solar.voltage;
    float chg = charge.voltage;
    
    Serial.printf("[MEASURE] Final voltages: Acc=%.3fV Sol=%.3fV Chg=%.3fV\n\n", 
                  acc, sol, chg);

    // ============================================================
    //    ТЕМПЕРАТУРНЫЕ ДАТЧИКИ
    // ============================================================
    Serial.printf("[MEASURE] Reading temperature sensors...\n");

    pinMode(PIR_AP_GND_SENS, OUTPUT);
    digitalWrite(PIR_AP_GND_SENS, 0);
    Wire.begin();
    OneWire ow(ONE_WIRE_BUS);
    DallasTemperature ds(&ow);
    ds.begin();
    ds.setResolution(9);

    ds.requestTemperatures();

    static Adafruit_BME280 b;
    static bool bOk = b.begin(0x76) || b.begin(0x77);
    float t_bme = bOk ? b.readTemperature() : -127;
    float hum   = bOk ? b.readHumidity()    : 0;
    float press = bOk ? b.readPressure() / 100.0F : 900;

    delay(70);
    float t_ds = ds.getTempCByIndex(0);
    if (t_ds == DEVICE_DISCONNECTED_C) t_ds = -127;
    
    pinMode(PIR_AP_GND_SENS, INPUT_PULLUP);
    
    Serial.printf("[MEASURE] Reading weight sensor...\n");
    float weight = readWeight();

    // ============================================================
    //    ПРОВЕРКА И СОХРАНЕНИЕ С НОВЫМИ ФОРМАТАМИ
    // ============================================================
    if (acc < 0) acc = 0;
    if (sol < 0) sol = 0;
    if (chg < 0) chg = 0;

    HourRecord& h = rtcHour(rtc.wakeHour);
    
    // DS18B20: с точностью 0.5°C (диапазон -63.5..63.5)
    h.t_ds5 = (t_ds < -63.5) ? -127 : constrain(round(t_ds * 2), -127, 127);
    
    // BME280: с точностью 0.5°C
    h.t_bme5 = (t_bme < -63.5) ? -127 : constrain(round(t_bme * 2), -127, 127);
    
    // Влажность: с точностью 0.2% (0-51%)
h.hum5 = (uint8_t)constrain((int)lroundf(hum), 0, 100);
    
    // Давление: смещение от 900 гПа
    h.pressDelta = (press >= 900) ? constrain(round(press - 900), 0, 255) : 0;
    
    // Напряжения: с точностью 0.02В (0-5.1В)
    h.acc50 = constrain(round(acc * 50), 0, 255);
    h.solar50 = constrain(round(sol * 50), 0, 255);
    h.charge50 = constrain(round(chg * 50), 0, 255);
    
    // Вес: с точностью 0.5 кг
    h.weight2 = constrain(round(weight * 2), 0, 255);

    rtcSave();

    // Вывод в понятном виде
    Serial.printf("[MEASURE] ✓ Hour %u complete:\n", rtc.wakeHour);
    Serial.printf("  DS18B20:  %.1f°C (raw: %d)\n", t_ds, h.t_ds5);
    Serial.printf("  BME280:   %.1f°C (raw: %d)\n", t_bme, h.t_bme5);
    Serial.printf("  Humidity: %.0f%% (raw: %d)\n", hum, h.hum5);
    Serial.printf("  Pressure: %.0f hPa (delta: %d)\n", press, h.pressDelta);
    Serial.printf("  Acc:      %.2fV (raw: %d)\n", acc, h.acc50);
    Serial.printf("  Solar:    %.2fV (raw: %d)\n", sol, h.solar50);
    Serial.printf("  Charge:   %.2fV (raw: %d)\n", chg, h.charge50);
    Serial.printf("  Weight:   %.2f kg (raw: %d)\n\n", weight, h.weight2);
}
uint32_t correctedHourlyTimestamp(int hourIndex, float temp) {
    uint32_t ts = rtc.baseTimestamp + hourIndex*3600;
    return ts;
}
// ===============================================================
//                    ПОСТРОЕНИЕ SMS С ПОЧАСОВЫМИ ДАННЫМИ (ОБРАТНЫЙ ПОРЯДОК)
// ===============================================================
String buildHourlyDataSMS() {
    String result = "";
    result.reserve(580);

    // 1) Порядковый номер: 3 знака с ведущими нулями (dailySendCount)
    char seq[4];
    snprintf(seq, sizeof(seq), "%03u", (unsigned)(rtc.dailySendCount));
    result += seq;

    // 2) Буква V
    result += "V";

    // 3) Версия: последние 9 знаков OTA_VERSION
    String ver = String(OTA_VERSION);
    if (ver.length() > 9) ver = ver.substring(ver.length() - 9);
    result += ver;

    int validHours = 0;

    for (int h = 23; h >= 0; h--) {
        HourRecord& r = rtcHour(h);

        // Метка перед данными Hour 0 (БЕЗ пробела)
        if (h == 0) {
            result += "H0";
        }

        float t_bme = (r.t_bme5 == -127) ? -127 : r.t_bme5 / 2.0f;
        float acc   = r.acc50 / 50.0f;

        bool empty = (r.t_bme5 == 0 && r.acc50 == 0);

        // Валидность часа теперь определяем ТОЛЬКО по АКБ (и empty)
        bool invalidBattery = (acc < 0.5f) || (acc > 6.0f);
        bool hourValid = (!empty && !invalidBattery);

        // Разделитель '-' только если температура реально есть и < 0
        bool hasTemp = (t_bme != -127) && hourValid;
        bool negTemp = hasTemp && (t_bme < 0.0f);

        if (!hourValid) {
            // пусто или плохая АКБ
            result += "-,-";
        } else {
            // АКБ валидна -> печатаем acc всегда
            int accCentivolts = (int)((acc - (int)acc) * 100 + 0.5);
            String accStr = (accCentivolts < 10) ? ("0" + String(accCentivolts)) : String(accCentivolts);

            if (t_bme == -127) {
                // BME нет -> температура отсутствует
                result += "-," + accStr;
            } else {
                // BME есть
                result += String((int)round(t_bme)) + "," + accStr;
            }
        }

        validHours++;

        // Разделитель между часами (кроме последнего)
        if (validHours < 24) {
            result += (negTemp ? "-" : " ");
        }

        // СТРОГО: итоговая длина SMS не больше 160 символов (включая "...")
        if (validHours < 24 && result.length() > 160) {
            if (result.length() > 157) result = result.substring(0, 157);
            result += "...";
            break;
        }
    }

    if (validHours == 0) return "NO DATA";
    return result;
}
// V14 diff-packed SMS, printable via ASCII offset (byte+32)
// Header bases are stored as (base + 40) to avoid control chars collisions.

static inline bool isHourEmpty(const HourRecord& r) {
    return (r.t_ds5 == 0) && (r.t_bme5 == 0) && (r.hum5 == 0) &&
           (r.pressDelta == 0) && (r.acc50 == 0) && (r.solar50 == 0) &&
           (r.weight2 == 0);
}

// Подстройте критерий валидности под ваши реалии.
// Важно: базы должны считаться по тем же часам, которые вы потом упакуете.
static inline bool isValidHour(const HourRecord& r) {
    if (isHourEmpty(r)) return false;

    // Отбрасываем "нет датчика"
    if (r.t_ds5 == -127 || r.t_bme5 == -127) return false;

    // Батарея должна быть вменяемая (иначе мусор)
    // acc50 = V * 50. 2.0V => 100
    if (r.acc50 < 100) return false;

    return true;
}
// ===============================================================
//                   ПОЛНЫЙ ОТЧЕТ ДЛЯ SMS (build_SMS())
// ===============================================================
// ======================= 6-bit printable codec =======================
// Alphabet size = 64, all ASCII and GSM-friendly
static const char ALPH64[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_";

static inline int alph64Index(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
    if (c >= 'a' && c <= 'z') return 36 + (c - 'a');
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static String encode6bit(const uint8_t* data, size_t len) {
    String out;
    out.reserve((len * 8 + 5) / 6); // ceil(bits/6)

    uint32_t acc = 0;
    uint8_t accBits = 0;

    for (size_t i = 0; i < len; i++) {
        acc = (acc << 8) | data[i];
        accBits += 8;

        while (accBits >= 6) {
            uint8_t v = (acc >> (accBits - 6)) & 0x3F;
            accBits -= 6;
            out += ALPH64[v];
        }
    }

    if (accBits > 0) {
        uint8_t v = (acc << (6 - accBits)) & 0x3F; // pad with 0
        out += ALPH64[v];
    }

    return out;
}
// date.cpp (add near other SIM800 helpers, before buildDiffSMS6())

static String sim800Stamp_MDDHHMM() {
    // Default if GSM disabled / SIM missing / parse fail
    String out = "000000";

    if (!cfg.gsmEnabled || !isSim800Present()) return out;
    if (!sim800Init()) return out;

    String r = sim800Command("AT+CCLK?", 1200);

    int q1 = r.indexOf('"');
    if (q1 == -1) return out;
    int q2 = r.indexOf('"', q1 + 1);
    if (q2 == -1) return out;

    // Example: 26/04/12,13:26:53+12
    String dt = r.substring(q1 + 1, q2);

    int comma = dt.indexOf(',');
    if (comma == -1) return out;

    String d = dt.substring(0, comma);      // "YY/MM/DD"
    String t = dt.substring(comma + 1);     // "HH:MM:SS+12"

    if (d.length() < 8 || t.length() < 5) return out;

    int mm = d.substring(3, 5).toInt();
    int dd = d.substring(6, 8).toInt();

    int hh = t.substring(0, 2).toInt();
    int mi = t.substring(3, 5).toInt();

    if (mm < 1 || mm > 12 || dd < 1 || dd > 31 || hh < 0 || hh > 23 || mi < 0 || mi > 59)
        return out;

    // MDDHHMM (month without leading zero, day/hh/mm with leading zeros)
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%02d%02d%02d", mm, dd, hh, mi);
    return String(buf);
}
String buildDiffSMS6() {
    // ======================= 6-bit printable codec =======================
    static const char ALPH64[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_";

    auto encode6bit = [&](const uint8_t* data, size_t len) -> String {
        String out;
        out.reserve((len * 8 + 5) / 6); // ceil(bits/6)

        uint32_t acc = 0;
        uint8_t accBits = 0;

        for (size_t i = 0; i < len; i++) {
            acc = (acc << 8) | data[i];
            accBits += 8;

            while (accBits >= 6) {
                uint8_t v = (acc >> (accBits - 6)) & 0x3F;
                accBits -= 6;
                out += ALPH64[v];
            }
        }

        if (accBits > 0) {
            uint8_t v = (acc << (6 - accBits)) & 0x3F; // pad with 0
            out += ALPH64[v];
        }

        return out;
    };

    auto isHourEmpty = [&](const HourRecord& r) -> bool {
        return (r.t_ds5 == 0) && (r.t_bme5 == 0) && (r.hum5 == 0) &&
               (r.pressDelta == 0) && (r.acc50 == 0) && (r.solar50 == 0) &&
               (r.weight2 == 0);
    };

    auto isValidHour = [&](const HourRecord& r) -> bool {
        if (isHourEmpty(r)) return false;
        // battery sanity (acc50 = V * 50; 2.0V => 100)
        if (r.acc50 < 100) return false;
        return true;
    };

    // ======================= Report number (3 digits) =======================
    char seq[4];
    snprintf(seq, sizeof(seq), "%03u", (unsigned)(rtc.dailySendCount));

    // ======================= Header voltage field (SIM800 battery) =======================
    float hdrV = 0.0f;
    if (g_lastSim800BattMv > 0) {
        hdrV = g_lastSim800BattMv / 1000.0f; // mV -> V
    } else {
        hdrV = rtcHour(23).acc50 / 50.0f;    // fallback
    }
    if (!(hdrV >= 0.0f && hdrV <= 10.0f)) hdrV = 0.0f;

    char battBuf[12];
    snprintf(battBuf, sizeof(battBuf), "%.2f", hdrV);

    int csq = (g_lastSim800CSQ >= 0) ? (int)g_lastSim800CSQ : 99;

    // ======================= Hour 00 extra human block (WITHOUT Solar) =======================
    const HourRecord& h00 = rtcHourConst(0);

    auto fmtTempIntOrDash = [&](int8_t t5) -> String {
        if (t5 == -127) return "-";
        int tC = (int)lroundf(((float)t5) / 2.0f);
        return String(tC);
    };

    String h00_tds  = fmtTempIntOrDash(h00.t_ds5);
    String h00_tbme = fmtTempIntOrDash(h00.t_bme5);

    int h00_press = 900 + (int)h00.pressDelta;
    int h00_hum = (int)h00.hum5;

    float h00_weight = h00.weight2 / 2.0f;

    char wBuf[12];
    snprintf(wBuf, sizeof(wBuf), "%.1f", h00_weight);

    // ======================= Find bases =======================
    bool hasAny = false;

    bool hasDs = false;
    bool hasBme = false;

    int8_t base_tds5  = 127;
    int8_t base_tbme5 = 127;

    uint8_t base_w2   = 255;
    uint8_t base_p    = 255;
    uint8_t base_a50  = 255;
    uint8_t base_s50  = 255;
    uint8_t base_h5   = 255;

    for (int h = 0; h < 24; h++) {
        const HourRecord& r = rtcHourConst(h);
        if (!isValidHour(r)) continue;
        hasAny = true;

        if (r.t_ds5 != -127)  { hasDs = true;  if (r.t_ds5  < base_tds5)  base_tds5  = r.t_ds5; }
        if (r.t_bme5 != -127) { hasBme = true; if (r.t_bme5 < base_tbme5) base_tbme5 = r.t_bme5; }

        if (r.weight2    < base_w2)   base_w2   = r.weight2;
        if (r.pressDelta < base_p)    base_p    = r.pressDelta;
        if (r.acc50      < base_a50)  base_a50  = r.acc50;
        if (r.solar50    < base_s50)  base_s50  = r.solar50;
        if (r.hum5       < base_h5)   base_h5   = r.hum5;
    }

    // If no valid hours => payload "-"
    String payload = "-";

    if (hasAny) {
        // If sensor never appeared => force base=0 and all deltas will be 15 ("-")
        if (!hasDs)  base_tds5  = 0;
        if (!hasBme) base_tbme5 = 0;

        int baseTdsC  = (int)lroundf(((float)base_tds5)  / 2.0f);
        int baseTbmeC = (int)lroundf(((float)base_tbme5) / 2.0f);

        // ======================= Build 79-byte packet =======================
        uint8_t pkt[7 + 24 * 3];
        size_t p = 0;

        // header (raw bytes)
        pkt[p++] = (uint8_t)base_tds5;
        pkt[p++] = (uint8_t)base_tbme5;
        pkt[p++] = base_w2;
        pkt[p++] = base_p;
        pkt[p++] = base_a50;
        pkt[p++] = base_s50;
        pkt[p++] = base_h5;

        for (int h = 0; h < 24; h++) {
            const HourRecord& r = rtcHourConst(h);

            uint8_t b0 = 0, b1 = 0, b2 = 0;

            if (isValidHour(r)) {
                // ---- temps: 15 means "no data" ("-") ----
                int d_tds = 15;
                int d_tbme = 15;

                if (hasDs && r.t_ds5 != -127) {
                    int tdsC = (int)lroundf(((float)r.t_ds5) / 2.0f);
                    d_tds = constrain(tdsC - baseTdsC, 0, 14);
                }
                if (hasBme && r.t_bme5 != -127) {
                    int tbmeC = (int)lroundf(((float)r.t_bme5) / 2.0f);
                    d_tbme = constrain(tbmeC - baseTbmeC, 0, 14);
                }

                b0 = (uint8_t)((d_tds << 4) | (d_tbme & 0x0F));

                // ---- weight + press ----
                float wKg     = r.weight2 * 0.5f;
                float baseWKg = base_w2   * 0.5f;
                int d_w = (int)lroundf((wKg - baseWKg) / 0.2f);
                d_w = constrain(d_w, 0, 15);

                int d_p = (int)lroundf(((int)r.pressDelta - (int)base_p) / 10.0f);
                d_p = constrain(d_p, 0, 15);

                b1 = (uint8_t)((d_w << 4) | (d_p & 0x0F));

                // ---- solar + hum + acc ----
                int solarV = (int)lroundf(r.solar50 * 0.02f);
                solarV = constrain(solarV, 0, 7);

                int humPct = (int)r.hum5;
                int humCode = (int)lroundf(((float)humPct - 30.0f) / 10.0f);
                humCode = constrain(humCode, 0, 7);

                float accV     = r.acc50  / 50.0f;
                float baseAccV = base_a50 / 50.0f;
                int d_acc = (int)lroundf((accV - baseAccV) / 0.1f);
                d_acc = constrain(d_acc, 0, 3);

                b2 = (uint8_t)((solarV << 5) | (humCode << 2) | (d_acc & 0x03));
            }

            pkt[p++] = b0;
            pkt[p++] = b1;
            pkt[p++] = b2;
        }

        // ======================= Encode payload =======================
        payload = encode6bit(pkt, sizeof(pkt));
        if (payload.length() == 0) payload = "-";
    }

    // ======================= Compose SMS (updated format) =======================
    // <SEQ>V<fw>,<bat>V,+CSQ,<csq>,<sleep>s,<Tds>,<Tbme>,<Press>,<Hum>,W<Weight>,<payload>
    String sms;
    sms.reserve(280 + payload.length());

    sms += seq;                     // report number
sms += "v";
{
    String ver = String(OTA_VERSION);
    // оставить только последние 4 символа
    if (ver.length() > 4) ver = ver.substring(ver.length() - 4);
    sms += ver;
}
    sms += "s";
    sms += String(csq);
    sms += ",";
    sms += battBuf;
    sms += "V";
sms += sim800Stamp_MDDHHMM();
sms += ",";
    sms += String((unsigned)SLEEP_SEC);
    sms += "S";
sms += ",";
    sms += "T";
    sms += h00_tds;
    sms += "in";
    sms += h00_tbme;
    sms += "p";
    sms += String(h00_press);
    sms += "h";
    sms += String(h00_hum);

    // NEW: "W<weight>," instead of "W00,<weight>,"
    sms += "w";
    sms += wBuf;

    sms += "=";
    sms += payload;

    return sms;
}

String build_SMS() {
    // MAC без двоеточий
    String mac = WiFi.macAddress();
    mac.replace(":", "");

    // GSM данные
    int gsmPct = 0, gsmMv = 0;
    String sDate = "--/--/--";
    String sTime = "--:--:--";
    int rssiGsm = -99;
    // Проверяем наличие SIM800 по флагу из RTC
    if (cfg.gsmEnabled && isSim800Present() && sim800Init()) {
        String d, t;
        if (sim800GetClock(t, d)) {
            if (t.length() >= 8) sTime = t;
            if (d.length() >= 8)
                sDate = d.substring(6) + "/" + d.substring(3,5) + "/" + d.substring(0,2);
        }
        if (sim800GetBattery(gsmPct, gsmMv)) {}
        int r = sim800GetRSSI();
        if (r >= 0) rssiGsm = r;
    } else {
        Serial.printf("[SMS] SIM800 not present or disabled, using default GSM values\n");
    }

    // NTP
    String ntpStr = "--:--:-- --/--/--";
    if (rtc.baseTimestamp > 1700000000UL) {
        time_t ts = rtc.baseTimestamp;
        char buf[32];
        strftime(buf, sizeof(buf), "%H:%M:%S %d/%m/%y", gmtime(&ts));
        ntpStr = buf;
    }
    (void)ESP.getFreeHeap();  // Вместо long heap = ESP.getFreeHeap();
    float run = millis() / 1000.0;

    // WiFi
    String wifiRssi = WiFi.isConnected() ? String(WiFi.RSSI()) : "-";
    String wifiIp   = WiFi.isConnected() ? WiFi.localIP().toString() : "-";
    String wifiSsid = WiFi.isConnected() ? WiFi.SSID() : "-";


    // === Последний час (23-й) — данные BME280 ===
    HourRecord& last = rtcHour(23);
    float t_bme = (last.t_bme5 == -127) ? -127 : last.t_bme5 / 2.0f;
    float hum = (float)last.hum5;
    float press = 900.0f + last.pressDelta;

    // Защита от мусора
    if (last.t_bme5 == -127 && last.hum5 == 0) {
        t_bme = -127; hum = 0; press = 0;
    }

    String bmeStr = (t_bme > -100) ?
        String(t_bme, 1) + "C," + String(hum, 0) + "%," + String(press, 0) + "h" :
        "-,-,-";

    // SMS — полностью в одну строку
    String sms = "";
    sms.reserve(400);

    sms += "V." + String(OTA_VERSION) + ",";
    sms += mac + ",";
    sms += "№" + String(rtc.dailySendCount + 1) + ",";
    sms += "Sleep" + String(cfg.sleepSec) + "s,";
    sms += "Run" + String(run, 0) + "s,";
    sms += "GSM" + sTime + " " + sDate + ",";
    sms += "RSSI" + String(rssiGsm) + ",";
    sms += "Bat" + String(gsmMv) + "mV,";
    sms += wifiSsid + ",";

    // только ЗНАЧЕНИЯ из config.json (без меток)
    sms += cfg.pass + ",";
    sms += cfg.sms + ",";
    sms += cfg.call + ",";
    sms += cfg.deviceName + ",";
    sms += (cfg.gsmEnabled ? "ON" : "OFF") + String(",");
    sms += "SIM" + String(isSim800Present() ? "1" : "0");

    return sms;
}

// ======================================================
//              ПОЛНЫЙ КРАСИВЫЙ ОТЧЁТ 
// ======================================================
String buildDailyReportText() {
    char tmp[128];
    char buf[32];

    auto formatLine = [&](const String& leftStr, const String& rightStr = "") {
        snprintf(tmp, sizeof(tmp), "%-36s%s\n", leftStr.c_str(), rightStr.c_str());
        return String(tmp);
    };

    String txt = "```\n";
    txt += "        📊 DAILY 24H REPORT\n";
    txt += "    ────────────────────────────────\n\n";

    // ===========================================================
    //              ИНФОРМАЦИЯ О СИСТЕМЕ
    // ===========================================================
    txt += "🆚 Version : " + String(OTA_VERSION);
    txt += " | MAC : " + WiFi.macAddress() + "\n";

    txt += "📤 Sent    : " + String(rtc.dailySendCount + 1);
    txt += " | 💤 Sleep : " + String(cfg.sleepSec) + "s (";
    txt += String((cfg.sleepSec * 24) / 3600.0, 1) + "h/day)\n";

    txt += "🧠 Heap    : " + String(ESP.getFreeHeap()) + " bytes";
    txt += " | ⏱️ Uptime : " + String(millis() / 1000.0, 1) + " sec\n\n";

    txt += "📁 OTA_PATH : " + String(OTA_PATH) + "\n";

    // ===========================================================
    //              ВРЕМЯ (NTP)
    // ===========================================================
    char ntpStr[32];
    if (rtc.baseTimestamp > 1700000000UL) {
        time_t ts = rtc.baseTimestamp;
        struct tm *tm = gmtime(&ts);
        strftime(ntpStr, sizeof(ntpStr), "%H:%M:%S %d/%m/%y", tm);
    } else {
        strcpy(ntpStr, "--:--:-- --/--/--");
    }

    // ===========================================================
    //              GSM ИНФОРМАЦИЯ
    // ===========================================================
    String simTimeStr = "--:--:-- --/--/--";
    String gsmRssiStr = "-";
    String gsmBattStr = "-";
    String gsmStatus = "OFF";

    if (cfg.gsmEnabled) {
        if (isSim800Present()) {
            gsmStatus = "PRESENT";
            if (sim800Init()) {
                String sDate, sTime;
                if (sim800GetClock(sTime, sDate)) {
                    String yy = sDate.substring(0, 2);
                    String mm = sDate.substring(3, 5);
                    String dd = sDate.substring(6, 8);

                    int signPos = sTime.indexOf('+');
                    if (signPos == -1) signPos = sTime.indexOf('-');
                    if (signPos > 0) {
                        sTime = sTime.substring(0, signPos);
                    }
                    if (sTime.length() == 7) sTime = "0" + sTime;
                    if (sTime.charAt(1) == ':') sTime = "0" + sTime;
                    simTimeStr = sTime + " " + dd + "." + mm + "." + yy;
                }

                // --- Battery (CBC) ---
                int gsmPct = 0, gsmMv = 0;
                if (sim800GetBattery(gsmPct, gsmMv)) {
                    gsmBattStr = String(gsmMv) + "mV " + String(gsmPct) + "%";
                    // NEW: persist for DiffSMS header
                    g_lastSim800BattMv = (uint16_t)constrain(gsmMv, 0, 65535);
                } else {
                    g_lastSim800BattMv = 0;
                }

                // --- CSQ ---
                int r = sim800GetRSSI();
                if (r >= 0) {
                    gsmRssiStr = String(r) + " (" + String(-113 + (r*2)) + " dBm)";
                    // NEW: persist for DiffSMS header
                    g_lastSim800CSQ = (int16_t)r;
                } else {
                    g_lastSim800CSQ = -1;
                }

            } else {
                simTimeStr = "NO NET";
                gsmStatus = "NO NET";
                g_lastSim800BattMv = 0;
                g_lastSim800CSQ = -1;
            }
        } else {
            simTimeStr = "NO SIM";
            gsmStatus = "NO SIM";
            g_lastSim800BattMv = 0;
            g_lastSim800CSQ = -1;
        }
    } else {
        simTimeStr = "DISABLED";
        gsmStatus = "DISABLED";
        g_lastSim800BattMv = 0;
        g_lastSim800CSQ = -1;
    }

    // ===========================================================
    //              WiFi ИНФОРМАЦИЯ
    // ===========================================================
    String wifiSsid = WiFi.isConnected() ? WiFi.SSID() : "-";
    String wifiIp   = WiFi.isConnected() ? WiFi.localIP().toString() : "-";
    String wifiRssi = WiFi.isConnected() ? String(WiFi.RSSI()) + " dBm" : "-";

    // ===========================================================
    //              СЕТЕВАЯ ИНФОРМАЦИЯ
    // ===========================================================
    txt += formatLine("🕒 NTP: " + String(ntpStr), "📱 GSM: " + simTimeStr);
    txt += formatLine("📶 WiFi RSSI: " + wifiRssi,
                      cfg.gsmEnabled ? ("📶 GSM RSSI: " + gsmRssiStr) : "");
    txt += formatLine("🆔 " + wifiSsid,
                      cfg.gsmEnabled ? ("🔋 " + gsmBattStr) : "");
    txt += formatLine("🌐 IP: " + wifiIp);
    txt += formatLine("📟 GSM Status: " + gsmStatus);
    txt += "\n";

    // ===========================================================
    //              WiFi SCAN LIST
    // ===========================================================
    if (wifiScanBeforeConnect.length() > 0) {
        txt += "📡 Available Networks:\n";
        int pos = wifiScanBeforeConnect.indexOf('\n');
        if (pos != -1) pos++;
        while (pos < (int)wifiScanBeforeConnect.length()) {
            int end = wifiScanBeforeConnect.indexOf('\n', pos);
            if (end == -1) end = (int)wifiScanBeforeConnect.length();
            String line = wifiScanBeforeConnect.substring(pos, end);
            line.trim();
            int cut = line.lastIndexOf(" -");
            if (cut != -1) line = line.substring(0, cut);
            line.replace("[LOCK]", "🔒");
            line.replace("[OPEN]", "🔓");
            line.trim();
            if (line.length()) txt += "  " + line + "\n";
            pos = end + 1;
        }
        txt += "\n";
    }

    // ===========================================================
    //              НАСТРОЙКИ
    // ===========================================================
    txt += "⚙️ --- SETTINGS ---\n";

    txt += "🔄 OTA INTERVAL : ";
#ifdef OTA_CHECK_INTERVAL
    txt += String(OTA_CHECK_INTERVAL) + " cycles";
#else
    txt += "?";
#endif
    txt += "\n";

    txt += "📱 Device Name     : " + (cfg.deviceName.length() ? cfg.deviceName : "-") + "\n";
    txt += "📞 SMS Number      : " + (cfg.sms.length() ? cfg.sms : "-") + "\n";
    txt += "📞 Call Number     : " + (cfg.call.length() ? cfg.call : "-") + "\n";
    txt += "🔑 WiFi Password   : " + (cfg.pass.length() ? cfg.pass : "-") + "\n";
    txt += "📶 WiFi SSID 1     : " + (cfg.wifi1.length() ? cfg.wifi1 : "-") + "\n";
    txt += "📶 WiFi SSID 2     : " + (cfg.wifi2.length() ? cfg.wifi2 : "-") + "\n";
    txt += "📶 WiFi SSID 3     : " + (cfg.wifi3.length() ? cfg.wifi3 : "-") + "\n";
    txt += "📶 WiFi SSID 4     : " + (cfg.wifi4.length() ? cfg.wifi4 : "-") + "\n";
    txt += "💤 Sleep Seconds   : " + String(cfg.sleepSec) + "\n";
    txt += "⚖️ Scale K         : " + String(cfg.scaleK, 4) + "\n";
    txt += "⚡ ATT K            : " + String(cfg.voltK, 4) + "\n";
    txt += "📟 GSM             : " + String(cfg.gsmEnabled ? "Enabled" : "Disabled") + "\n";
    txt += "🔋 Low Battery Mode: " + String(isLowBatteryMode() ? "YES" : "NO");
    if (isLowBatteryMode()) {
        txt += " (" + String(rtc.lowBatteryHours) + " days)";
    }
    txt += "\n\n";

    // ===========================================================
    //              ПОЧАСОВЫЕ ДАННЫЕ
    // ===========================================================
    txt += "📊 HOURLY DATA:\n";
    txt += "Hr  T_DS  T_BME  Hum  ΔP   Acc   Sol   Chg   kg\n";
    txt += "─── ───── ───── ──── ──── ───── ───── ───── ─────\n";

    float minT = 999, maxT = -999, sumT = 0;
    int validDs = 0;
    int validHours = 0;

    for (int h = 0; h < 24; h++) {
        HourRecord &r = rtcHour(h);

        // Конвертируем из сжатых форматов
        float t_ds = (r.t_ds5 == -127) ? -127 : r.t_ds5 / 2.0f;
        float t_bme = (r.t_bme5 == -127) ? -127 : r.t_bme5 / 2.0f;
        float hum = (float)r.hum5;
        float press = 900.0f + r.pressDelta;
        float acc = r.acc50 / 50.0f;
        float sol = r.solar50 / 50.0f;
        float ch = r.charge50 / 50.0f;
        float w = r.weight2 / 2.0f;

        // Проверка на пустую запись
        bool empty = (r.t_ds5 == 0 && r.t_bme5 == 0 && r.hum5 == 0 &&
                      r.pressDelta == 0 && r.acc50 == 0 && r.solar50 == 0 &&
                      r.charge50 == 0 && r.weight2 == 0);

        if (empty) {
            snprintf(tmp, sizeof(tmp),
                     "%02d  ----- ----- ---- ----  ----- ----- ----- -----\n", h);
            txt += tmp;
            continue;
        }

        validHours++;

        char ds[7], bme[7];
        if (r.t_ds5 == -127) {
            strcpy(ds, " --- ");
        } else {
            snprintf(ds, sizeof(ds), "%5.1f", t_ds);
            if (t_ds > -100) {
                minT = min(minT, t_ds);
                maxT = max(maxT, t_ds);
                sumT += t_ds;
                validDs++;
            }
        }

        if (r.t_bme5 == -127) {
            strcpy(bme, " --- ");
        } else {
            snprintf(bme, sizeof(bme), "%5.1f", t_bme);
        }

        snprintf(tmp, sizeof(tmp),
                 "%02d  %5s %5s %4.0f %4d %5.1f %5.1f %5.1f %5.2f\n",
                 h, ds, bme, hum, r.pressDelta, acc, sol, ch, w);

        txt += tmp;
    }

    txt += "──────────────────────────────────────────────────────────\n";

    if (validHours == 0) {
        txt += "📭 No hourly measurements yet\n";
    }

    if (validDs > 0) {
        char avgStr[16];
        snprintf(avgStr, sizeof(avgStr), "%.1f", sumT / validDs);
        txt += "🌡️ DS18B20: Min " + String(minT, 1) + "°C";
        txt += " | Max " + String(maxT, 1) + "°C";
        txt += " | Avg " + String(avgStr) + "°C\n";
    }

    // Добавляем информацию о последнем Wi-Fi подключении
    if (rtc.wifiChannel > 0) {
        txt += "📶 Last WiFi: Channel " + String(rtc.wifiChannel);
        txt += " | BSSID: ";
        for (int i = 0; i < 6; i++) {
            if (i > 0) txt += ":";
            snprintf(buf, sizeof(buf), "%02X", rtc.wifiBssid[i]);
            txt += buf;
        }
        txt += "\n";
    }

    txt += "```";
    return txt;
}
// Добавляем extern для sendDailyReport в начале файла
extern bool sendDailyReport(const String& extra);
// ===============================================================
//                   SAVE RTC TO FS WHEN NO NET
// ===============================================================

void saveRTCToLittleFS() {
    Serial.printf("[FS] Saving 24h RTC data to LittleFS...");
    if (!LittleFS.begin()) {
        Serial.printf("[FS] LittleFS failed to start!");
        return;
    }

    char fname[32];
    sprintf(fname, "/day_%u.bin", rtc.baseTimestamp);

    File f = LittleFS.open(fname, "w");
    if(!f) {
        Serial.printf("[FS] Failed to create file: %s\n", fname);
        return;
    }

    f.write((uint8_t*)rtc.hours, sizeof(rtc.hours));
    f.close();
    Serial.printf("[FS] Saved %zu bytes of historical data to %s\n", sizeof(rtc.hours), fname);
}

// ===============================================================
//                        HOURLY CYCLE
// ===============================================================
void hourlyCycle() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.println("=== HOURLY CYCLE START ===");

    // Измеряем и сохраняем ТЕКУЩИЙ час: rtc.wakeHour (1..23)
    measureAndSaveHour();

    HourRecord& h = rtcHour(rtc.wakeHour);
    float Acc = h.acc50 / 50.0f;

    if (!isLowBatteryMode() && Acc <= 3.0f) {
        setLowBatteryMode(true);
        rtc.lowBatteryHours = 0;
        Serial.println("[POWER] → ENTER LOW BATTERY MODE (≤3.0V)");
    }

    if (isLowBatteryMode() && Acc > 3.6f) {
        setLowBatteryMode(false);
        rtc.lowBatteryHours = 0;
        Serial.println("[POWER] ← EXIT LOW BATTERY MODE (>3.6V)");
    }

    // ВАЖНО: инкремент часа делаем ПОСЛЕ измерения,
    // чтобы не пропускать Hour 1 и далее.
    rtc.wakeHour++;
    rtcSave();

    pinMode(WAKE_SLEEP, OUTPUT);
    digitalWrite(WAKE_SLEEP, HIGH);
    Serial.flush();
    ESP.deepSleep(cfg.sleepSec * 1000000ULL, WAKE_RF_DEFAULT);
}
void OTA() {
    // Режим повторных попыток (если прошлый раз обновление не стартовало/упало)
    static bool retryMode = false;

    // ==========================
    // 1) Решаем: делать OTA сейчас или нет
    // ==========================
    bool doOtaThisCycle = false;

    if (retryMode) {
        doOtaThisCycle = true;
        Serial.println("[OTA] retryMode=ON -> checking every cycle");
    } else {
        if (OTA_CHECK_INTERVAL == 1) {
            doOtaThisCycle = true;
        } else {
            if ((rtc.dailySendCount % OTA_CHECK_INTERVAL) == 0) {
                doOtaThisCycle = true;
            }
        }
    }

    if (!doOtaThisCycle) {
        uint32_t remCycles = OTA_CHECK_INTERVAL - (rtc.dailySendCount % OTA_CHECK_INTERVAL);
        if (remCycles == OTA_CHECK_INTERVAL) remCycles = 0;
        Serial.printf("[OTA] Skipped, next check in %u cycles (dailySendCount=%u, interval=%u)\n",
                      remCycles, rtc.dailySendCount, OTA_CHECK_INTERVAL);
        return;
    }

    Serial.printf("[OTA] START (dailySendCount=%u, interval=%u, retryMode=%s)\n",
                  rtc.dailySendCount, OTA_CHECK_INTERVAL, retryMode ? "ON" : "OFF");

    // ==========================
    // 2) Максимальная очистка RAM / сетевых хвостов
    // ==========================
    uint32_t heap0 = ESP.getFreeHeap();
    Serial.printf("[OTA] 1 Free heap BEFORE cleanup: %u\n", heap0);

    // Остановить все возможные WiFi-клиенты
    WiFiClient::stopAll();
    delay(5);
    yield();

    // Завершить SoftwareSerial SIM800 (освобождает буферы)
    sim800.flush();
    sim800.end();
    delay(5);
    yield();

    // Освободить большие String (скан лог иногда жирный)
    wifiScanBeforeConnect = "";
    // shrinkToFit() есть не во всех ядрах/сборках; если не компилится — удалите строку ниже.

    // Полностью выключить WiFi стек и включить заново (часто возвращает куски heap)
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(50);
    yield();

    system_soft_wdt_feed();
    ESP.wdtFeed();

    uint32_t heap1 = ESP.getFreeHeap();
    Serial.printf("[OTA] Free heap AFTER cleanup: %u (+%d)\n", heap1, (int)(heap1 - heap0));

    // ==========================
    // 3) Подключение к WiFi только для OTA
    // ==========================
    Serial.println("[OTA] Connecting to WiFi for OTA check...");

    String bestSSID, scanLog;
    int bestRSSI = -999, bestChannel = 0;
    uint8_t bestBSSID[6] = {0};

    bool wifiConnected = wifi_connect(bestSSID, bestRSSI, bestChannel, bestBSSID, scanLog);
    wifiScanBeforeConnect = scanLog;

    if (!wifiConnected) {
        Serial.println("[OTA] WiFi connect FAILED -> OTA skipped");
        // В режиме retryMode оставляем retryMode=true, чтобы попробовать снова в след. цикле
        retryMode = true;
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return;
    }

    Serial.printf("[OTA] WiFi OK: SSID=%s RSSI=%d IP=%s\n",
                  bestSSID.c_str(), bestRSSI, WiFi.localIP().toString().c_str());

    Serial.printf("[OTA] Free heap BEFORE checkUpdate: %u\n", ESP.getFreeHeap());

    // ==========================
    // 4) Проверка и запуск обновления
    // ==========================
    String ver, notes;
    bool hasUpdate = ota.checkUpdate(&ver, &notes);

    if (!hasUpdate) {
        Serial.println("[OTA] No update available");
        // Если мы были в retryMode, но обновлений нет — выключаем retryMode
        retryMode = false;

        // Немного “протикать” (иногда нужно библиотеке)
        unsigned long start = millis();
        while (millis() - start < 800) {
            ota.tick();
            yield();
            delay(10);
        }

        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        Serial.printf("[OTA] END (no update). Free heap: %u\n", ESP.getFreeHeap());
        return;
    }

    Serial.printf("[OTA] UPDATE FOUND: %s\n", ver.c_str());
    if (notes.length()) {
        Serial.printf("[OTA] Notes: %s\n", notes.c_str());
    }

    // Старт обновления
    ota.update();

    // ==========================
    // 5) Ждём завершения через tick()
    // ==========================
    const unsigned long WAIT_MS = 60000;
    unsigned long t0 = millis();
    bool started = false;

    Serial.println("[OTA] Waiting for OTA tick...");

    while (millis() - t0 < WAIT_MS) {
        if (ota.tick()) {
            int err = (int)ota.getError();
            // В AutoOTA часто 0 = OK/идёт процесс, ненулевые = ошибки/статусы
            if (err == 0) started = true;

            // Логируем изменения состояния (чтобы не спамить)
            static int lastErr = -999;
            if (err != lastErr) {
                lastErr = err;
                Serial.printf("[OTA] tick err/state = %d\n", err);
            }

            // Если библиотека сама перезагрузит устройство — сюда обычно уже не дойдём
        }

        yield();
        delay(50);
    }

    // Если мы дошли сюда — значит устройство не перезагрузилось само
    if (!started) {
        Serial.println("[OTA] Update did NOT start -> enabling retryMode");
        retryMode = true;
    } else {
        Serial.println("[OTA] Tick loop finished (device did not reboot automatically)");
        // На всякий случай можно принудительно рестартануть
        // ESP.restart();
        retryMode = true; // или false — зависит от желаемой логики
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    Serial.printf("[OTA] END. Free heap: %u\n", ESP.getFreeHeap());
}
// ===============================================================
//                        DAILY CYCLE 
// ===============================================================
void dailyCycle() {
    Serial.printf("\n=== DAILY CYCLE START (Hour 24) ===\n");
    uint32_t cycleStart = millis();

    // ===========================================================
    //              ИНИЦИАЛИЗАЦИЯ И СЧЕТЧИКИ
    // ===========================================================
    Serial.printf("[DAILY] dailySendCount BEFORE increment: %u\n", rtc.dailySendCount);

    // Включаем питание SIM800 только если флаг присутствия = true
    if (isSim800Present()) {
        digitalWrite(GSM_POW_Ring_Zamok, HIGH);
        Serial.printf("[DAILY] SIM800 power ON (present flag = true)\n");
        delay(100);
    } else {
        Serial.printf("[DAILY] SIM800 power NOT enabled (present flag = false)\n");
    }

    // Увеличиваем счетчик для OTA
    rtc.dailySendCount++;
    rtcSave();
    Serial.printf("[DAILY] dailySendCount AFTER increment: %u\n", rtc.dailySendCount);

    // ===========================================================
    //              ПЕРЕМЕННЫЕ ДЛЯ СИНХРОНИЗАЦИИ
    // ===========================================================
    bool timeSynced    = false;
    uint32_t realTS    = 0;
    bool wifiConnected = false;
    bool gsmFallback   = false;

    Serial.printf("[DAILY] Current baseTimestamp = %u (%s)\n",
                  rtc.baseTimestamp,
                  (rtc.baseTimestamp > 1700000000UL) ? "valid-ish" : "probably invalid");

    // ===========================================================
    //              ПОДКЛЮЧЕНИЕ К WIFI
    // ===========================================================
    Serial.printf("[DAILY] Scanning and connecting to WiFi...\n");

    String bestSSID, scanLog;
    int bestRSSI = -999, bestChannel = 0;
    uint8_t bestBSSID[6] = {0};

    wifiConnected = wifi_connect(bestSSID, bestRSSI, bestChannel, bestBSSID, scanLog);
    wifiScanBeforeConnect = scanLog;
    gsmFallback = !wifiConnected;

    if (wifiConnected) {
        Serial.printf("[WiFi] Connected to %s (RSSI: %d dBm, Channel: %d)\n",
                      bestSSID.c_str(), bestRSSI, bestChannel);
    } else {
        Serial.printf("[WiFi] No connection - will use GSM fallback if available\n");
    }

    // ===========================================================
    //              ПРОВЕРКА LOW BATTERY MODE
    // ===========================================================
    HourRecord& DH = rtcHour(23);
    float Acc = DH.acc50 / 50.0f;

    Serial.printf("[POWER] Last hour battery: %.3fV\n", Acc);
    Serial.printf("[POWER] lowBatteryMode = %s\n", isLowBatteryMode() ? "true" : "false");
    Serial.printf("[POWER] lowBatteryHours = %u\n", rtc.lowBatteryHours);

    if (isLowBatteryMode()) {
        if (Acc > 3.6f) {
            Serial.printf("[POWER] ✅ SUCCESS: Battery recovered: %.3fV → EXITING low battery mode\n", Acc);
            setLowBatteryMode(false);
            rtc.lowBatteryHours = 0;
            rtcSave();
            Serial.printf("[POWER] Low battery mode disabled\n");
        } else {
            Serial.printf("[POWER] ⚠️ WARNING: Still in low battery (%.3fV), days in LBM: %u\n",
                          Acc, rtc.lowBatteryHours + 1);

            if (rtc.lowBatteryHours == 0) {
                Serial.printf("[POWER] FIRST LBM DAY → sending warning\n");
                String warn = "⚠️ LOW BATTERY MODE ACTIVATED\n";
                warn += "Battery: " + String(Acc, 3) + "V\n";
                warn += "Day: " + String(rtc.dailySendCount) + "\n";
                warn += "Will skip full reports until recovery (>3.6V)";

                sendDailyReport(warn);
            } else {
                Serial.printf("[POWER] LBM active → skipping full report\n");
            }

            rtc.lowBatteryHours++;
            rtc.wakeHour = 0;
            rtcSave();

            Serial.printf("[POWER] Measuring hour 0 before sleep...\n");
            measureAndSaveHour(); // уже пишет в rtcHour(0)

            Serial.printf("[DAILY] Entering deep sleep (LBM active)...\n");
            Serial.flush();

            pinMode(WAKE_SLEEP, OUTPUT);
            digitalWrite(WAKE_SLEEP, HIGH);
            Serial.flush();

            ESP.deepSleep(cfg.sleepSec * 1000000ULL, WAKE_RF_DEFAULT);
            return;
        }
    }

    // ===========================================================
    //              СИНХРОНИЗАЦИЯ ВРЕМЕНИ
    // ===========================================================
    // 1) NTP (если WiFi есть)
    if (wifiConnected) {
        Serial.printf("[TIME] Attempting NTP time sync...\n");

        WiFiUDP ntpUDP;
        NTPClient ntp(ntpUDP, "pool.ntp.org");
        ntp.begin();
        ntp.setTimeOffset(0); // UTC epoch

        uint32_t tStart = millis();
        for (int i = 0; i < 3 && !timeSynced && (millis() - tStart < 8000); i++) {
            if (ntp.forceUpdate()) {
                realTS = (uint32_t)ntp.getEpochTime();
                if (isSaneEpoch(realTS)) {
                    timeSynced = true;
                    Serial.printf("[TIME] ✅ NTP synced! Timestamp(UTC) = %u\n", realTS);
                } else {
                    Serial.printf("[TIME] ❌ NTP returned insane epoch: %u\n", realTS);
                    realTS = 0;
                }
            } else {
                Serial.printf("[TIME] NTP attempt %d failed, retrying...\n", i + 1);
                delay(1000);
            }
        }

        if (!timeSynced) {
            Serial.printf("[TIME] ⚠️ All NTP attempts failed\n");
        }
    }

    // 2) SIM800 fallback (если NTP не удалось)
    if (!timeSynced) {
        uint32_t gsmEpoch = 0;
        Serial.printf("[TIME] Trying GSM(SIM800) epoch fallback...\n");

        if (getEpochFromSim800(gsmEpoch)) {
            realTS = gsmEpoch;          // already UTC epoch
            timeSynced = true;
            Serial.printf("[TIME] ✅ GSM time accepted. Epoch(UTC) = %u\n", realTS);
        } else {
            Serial.printf("[TIME] ❌ GSM time not available/invalid\n");
        }
    }

    // 3) если вообще ничего — оценка (но только если старый baseTimestamp sane)
    if (!timeSynced) {
        if (isSaneEpoch(rtc.baseTimestamp)) {
            Serial.printf("[TIME] ⚠️ Using estimated time (baseTimestamp + 24h)\n");
            realTS = rtc.baseTimestamp + 24UL * 3600UL;
            if (!isSaneEpoch(realTS)) realTS = 0;
        } else {
            Serial.printf("[TIME] ⚠️ No sane time source (rtc.baseTimestamp also invalid). Keeping 0\n");
            realTS = 0;
        }
    }

    // ===========================================================
    //              СОХРАНЕНИЕ НОВОГО ВРЕМЕНИ
    // ===========================================================
    if (isSaneEpoch(realTS)) {
        Serial.printf("[TIME] Saving new baseTimestamp(UTC) = %u to RTC\n", realTS);
        rtc.baseTimestamp = realTS;
    } else {
        Serial.printf("[TIME] Not saving baseTimestamp because it's invalid: %u\n", realTS);
        rtc.baseTimestamp = 0;
    }

    // ✅ NEW: перед дневным отчётом "текущий замер" должен попасть в Hour 0
    rtc.wakeHour = 0;
    rtcSave();

    Serial.printf("[DAILY] ✅ Measuring current data into Hour 0 before report...\n");
    measureAndSaveHour(); // внутри делает rtcSave()

    // (wakeHour оставляем 0 — это удобно для старта нового дня)
    // rtcSave() уже был внутри measureAndSaveHour()

    // ===========================================================
    //              ОТПРАВКА ДНЕВНОГО ОТЧЕТА
    // ===========================================================
    Serial.printf("[REPORT] gsmFallback = %s\n", gsmFallback ? "true" : "false");
    Serial.printf("[REPORT] Calling sendDailyReport()...\n");

    uint32_t reportStart = millis();
    bool sent = sendDailyReport("");
    uint32_t reportTime = millis() - reportStart;

    Serial.printf("[REPORT] sendDailyReport() returned: %s (took %lu ms)\n",
                  sent ? "OK" : "FAILED",
                  (unsigned long)reportTime);

    // ===========================================================
    //              СОХРАНЕНИЕ В LittleFS (АРХИВ)
    // ===========================================================
    Serial.printf("[ARCHIVE] Saving daily data to LittleFS...\n");

    if (!LittleFS.begin()) {
        Serial.printf("[ARCHIVE] ❌ ERROR: LittleFS.begin() failed!\n");
    } else {
        if (!isSaneEpoch(rtc.baseTimestamp)) {
            Serial.printf("[ARCHIVE] ❌ ERROR: baseTimestamp invalid (%u) -> archive skipped\n",
                          rtc.baseTimestamp);
        } else {
            char fname[32];
            snprintf(fname, sizeof(fname), "/day_%u.bin", (unsigned)rtc.baseTimestamp);

            File f = LittleFS.open(fname, "w");
            if (!f) {
                Serial.printf("[ARCHIVE] ❌ ERROR: Failed to create file %s\n", fname);
            } else {
                size_t written = f.write((uint8_t*)rtc.hours, sizeof(rtc.hours));
                f.close();

                if (written != sizeof(rtc.hours)) {
                    Serial.printf("[ARCHIVE] ❌ ERROR: Short write: %u/%u bytes to %s\n",
                                  (unsigned)written, (unsigned)sizeof(rtc.hours), fname);
                } else {
                    Serial.printf("[ARCHIVE] ✅ SUCCESS: Saved %u bytes to %s\n",
                                  (unsigned)written, fname);
                }
            }
        }
    }
digitalWrite(GSM_POW_Ring_Zamok, 0);
    // ===========================================================
    //              ПРОВЕРКА OTA
    // ===========================================================
    Serial.printf("[OTA] Checking for updates...\n");
    OTA();

    // ===========================================================
    //              ПЕРЕХОД В СОН
    // ===========================================================
    uint32_t totalCycleTime = millis() - cycleStart;
    Serial.printf("[DAILY] Total cycle time: %lu ms\n", (unsigned long)totalCycleTime);
    Serial.printf("[DAILY] DAILY CYCLE COMPLETED → deep sleep (%u seconds)\n", cfg.sleepSec);
    Serial.flush();

    pinMode(WAKE_SLEEP, OUTPUT);
    digitalWrite(WAKE_SLEEP, HIGH);
    Serial.flush();

    ESP.deepSleep(cfg.sleepSec * 1000000ULL, WAKE_RF_DEFAULT);
}
// ===============================================================
//                    SEND DAILY REPORT FUNCTION
// ===============================================================
bool sendDailyReport(const String& extra) {
    Serial.printf("[SEND] Preparing daily report...\n");

    bool wifiOK = WiFi.isConnected();
    bool gsmOK = false;

    // ================================
    // 1. GSM Init только если SIM800 присутствует
    // ================================
    if (cfg.gsmEnabled) {
        if (isSim800Present()) {
            Serial.printf("[SEND] SIM800 present according to RTC flag, initializing...\n");

            // 🔍 ПОЛНАЯ ОТЛАДКА SIM800 (Будит модем и чистит буфер)
            sim800Debug();

            // Теперь официальная инициализация
            gsmOK = sim800Init();

            if (gsmOK) {
                Serial.printf("[SEND] SIM800 initialized successfully\n");
            } else {
                Serial.printf("[SEND] SIM800 initialization FAILED\n");
            }
        } else {
            Serial.printf("[SEND] SIM800 not present according to RTC flag, GSM skipped.\n");
        }
    } else {
        Serial.printf("[SEND] GSM DISABLED in config.\n");
    }

    // ================================
    // 2. Build Report
    // ================================
    String report = buildDailyReportText();
    if (extra.length() > 0) {
        report += "\n" + extra;
    }

    Serial.printf("[SEND] Report assembled, length: %d chars\n", report.length());

    // ======================================================
    // 3. GSM → SMS
    // ======================================================

    if (cfg.gsmEnabled && gsmOK) {
        Serial.printf("[SEND] Sending hourly data SMS via GSM...\n");
#if DEBUG_GLOBAL
    rtcDebugDumpToSerial("before SMS");
#endif
        // Строим SMS с почасовыми данными в формате: "25,45 24,42 23,38 ..."
        String hourlyData = buildDiffSMS6();
        
        // Добавляем только данные, без заголовков
        String sms = hourlyData;
        
        // Ограничиваем длину SMS (обычно 160 символов)
        if (sms.length() > 160) {
            sms = sms.substring(0, 157) + "...";
        }
        
        // ============================================
        // ОТЛАДОЧНЫЙ ВЫВОД (ДУБЛИРУЕТСЯ, НО ДЛЯ НАДЕЖНОСТИ)
        // ============================================
        #if DEBUG_SIM800
            Serial.printf("\n[SEND_DEBUG] =========================================\n");
            Serial.printf("[SEND_DEBUG] Формирование SMS для отправки\n");
            Serial.printf("[SEND_DEBUG] Исходные данные (24 часа): %s\n", hourlyData.c_str());
            Serial.printf("[SEND_DEBUG] Финальное SMS (%d символов): %s\n", sms.length(), sms.c_str());
            Serial.printf("[SEND_DEBUG] =========================================\n\n");
        #endif
        
        bool s1 = sim800SendSMS(cfg.sms, sms);

        if (s1) {
            Serial.printf("[SEND] GSM SMS OK\n");
        } else {
            Serial.printf("[SEND] GSM SMS FAILED\n");
        }
    }
 /*    
    // ======================================================
    // 4. WiFi → Telegram
    // ======================================================
    if (wifiOK) {
        Serial.printf("[SEND] Sending via Telegram...\n");

        fb::Message m;
        m.chatID = CHAT_ID;
        m.text = report;

        fb::Result r = bot.sendMessage(m);

        if (r) {
            Serial.printf("[SEND] Telegram OK\n");
        } else {
            Serial.printf("[SEND] Telegram FAILED\n");
        }
    } else {
        Serial.printf("[SEND] WiFi NOT connected → Telegram skipped.\n");
    }
*/ 
    // Финальный статус
    Serial.printf("[SEND] Done.\n");
    return true;
  
}