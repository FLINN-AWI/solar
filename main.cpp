// BUILD_TIMESTAMP: 2026-04-04 21:54:41
// main.cpp — МАКСИМАЛЬНО УПРОЩЕННАЯ ВЕРСИЯ

#include <Arduino.h>
#include <LittleFS.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <AutoOTA.h>
#include <FastBot2.h>
#include <SoftwareSerial.h>
#include <time.h>
#include <user_interface.h>
#include "config.h"
#include "rtc.h"

// ===============================================================
//                    ОПРЕДЕЛЕНИЕ СТРУКТУР
// ===============================================================
struct TimeData {
    int hour;
    int min;
    int wday;
    bool inPeriod;
};

// Структура для результатов измерений каналов
struct ChannelResult {
    float voltage;
    float avgADC;
    int samples;
    const char* name;
};

// ===============================================================
//                    ГЛОБАЛЬНЫЕ ОБЪЕКТЫ
// ===============================================================
ConfigData cfg;
ESP8266WebServer HttpServer(SERVERPORT);
ESP8266HTTPUpdateServer httpUpdater;
SoftwareSerial sim800(SOL_GSM_RX, ACC_GSM_TX);
String wifiScanBeforeConnect;

// УБИРАЕМ ЭТУ СТРОКУ - rtc определена в rtc.cpp
// RTCStore rtc;

FastBot2 bot(BOT_TOKEN);
FastBot2 bot2(BOT2_TOKEN);

// ===============================================================
//                    ОБЪЯВЛЕНИЯ ФУНКЦИЙ ИЗ ДРУГИХ ФАЙЛОВ
// ===============================================================

// Из date.cpp
extern void measureAndSaveHour();
extern void hourlyCycle();
extern void dailyCycle();
extern void OTA();
extern void handleCalibrate();
extern void handleCalibrate2();
extern String buildDailyReportText();
extern bool sendDailyReport(const String& extra);
extern void sim800Debug();
extern ChannelResult measureSingleChannel(uint8_t pin, const char* channelName);
extern  void doOneTimeTare();
// Из net.cpp
extern bool wifi_connect(String& outBestSSID, int& outBestRSSI, int& outBestChannel, 
                         uint8_t outBestBSSID[6], String& scanLog);
extern void enterAPMode(uint32_t durationSec);
extern void ALARM();
extern bool sim800Init();
extern bool sim800SendSMS(const String& phone, const String& msg);
extern bool checkSim800Presence();
extern String sim800Command(const String& cmd, uint32_t timeout);

// Из viessmann.cpp
extern void handleViessmannMode();
extern void pulse(int pin, int ms);
extern TimeData handleTime(int mode, int startHour, int startMin, int endHour, int endMin);

// Функции работы с конфигом
extern bool loadConfig();
extern void saveConfig();

// Из _Domofon.cpp
extern void handleDomofonMode();

// ===============================================================
//                    СТАТИЧЕСКИЕ ПЕРЕМЕННЫЕ
// ===============================================================
static uint32_t bootMinutes = 0;

void resetConfigToDefaults() {
    cfg.pass = DEFAULT_PASS;
    cfg.wifi1 = DEFAULT_WIFI_AP_1;
    cfg.wifi2 = DEFAULT_WIFI_AP_2;
    cfg.wifi3 = DEFAULT_WIFI_AP_3;
    cfg.wifi4 = DEFAULT_WIFI_AP_4;
    cfg.sms = SMS_NUMBER;
    cfg.call = CALL_NUMBER1;
    cfg.sleepSec = SLEEP_SEC;
    cfg.deviceName = DEFAULT_DEVICE_NAME;
    cfg.gsmEnabled = DEFAULT_GSM_ENABLED;
    cfg.voltK = DEFAULT_VOLT_K;
    cfg.scaleK = DEFAULT_SCALE_K;
    saveConfig();
    Serial.println("Config reset to defaults");
}

// ===============================================================
//                    ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ===============================================================
void printBootInfo() {
    Serial.printf("\n\n=== WAKE UP ===\n");
    Serial.printf("Ver %s\n", OTA_VERSION);
    Serial.printf("Boot count: %u\n", ++bootMinutes);
    Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
    Serial.printf("Wake reason: %d\n", ESP.getResetInfoPtr()->reason);
}

void printRTCStatus() {
    Serial.printf("RTC: wakeHour=%u, baseTimestamp=%u, dailySendCount=%u\n", 
                  rtc.wakeHour, rtc.baseTimestamp, rtc.dailySendCount);
    Serial.printf("RTC: lowBatteryHours=%u, rehabHours=%u\n",
                  rtc.lowBatteryHours, rtc.rehabHours);
    Serial.printf("RTC: lowBatteryMode=%s, onExecuted=%s, offExecuted=%s, sim800Present=%s\n",
                  isLowBatteryMode() ? "yes" : "no",
                  isOnExecuted() ? "yes" : "no",
                  isOffExecuted() ? "yes" : "no",
                  isSim800Present() ? "yes" : "no");
}
void loadConfigWrapper() {
    if (!loadConfig()) {
        Serial.println("Config not found, using defaults");
        cfg.pass = DEFAULT_PASS;
        cfg.wifi1 = DEFAULT_WIFI_AP_1;
        cfg.wifi2 = DEFAULT_WIFI_AP_2;
        cfg.wifi3 = "";
        cfg.wifi4 = "";
        cfg.sms = SMS_NUMBER;
        cfg.call = CALL_NUMBER1;
        cfg.sleepSec = SLEEP_SEC;
        cfg.deviceName = DEFAULT_DEVICE_NAME;
        cfg.gsmEnabled = DEFAULT_GSM_ENABLED;
        cfg.voltK = DEFAULT_VOLT_K;
        cfg.scaleK = DEFAULT_SCALE_K;
    }
    
    // Отладка
    Serial.println("\n=== CONFIG LOADED ===");
    Serial.printf("WiFi1: %s\n", cfg.wifi1.c_str());
    Serial.printf("WiFi2: %s\n", cfg.wifi2.c_str());
    Serial.printf("WiFi3: %s\n", cfg.wifi3.c_str());
    Serial.printf("WiFi4: %s\n", cfg.wifi4.c_str());
    Serial.printf("Password length: %d\n", cfg.pass.length());
    Serial.println("=====================\n");
}
// ===============================================================
//                    ФУНКЦИЯ ГЕНЕРАЦИИ 1 КГЦ НА ONE_WIRE_BUS
// ===============================================================
void RingTone() {
    Serial.println("[PIR] Playing 'Ding-dong' on ONE_WIRE_BUS");
    
    pinMode(ONE_WIRE_BUS, OUTPUT);
    digitalWrite(ONE_WIRE_BUS, HIGH);
    
    int notes[] = {800, 1200};
    int durations[] = {300, 500};
    
    for (int i = 0; i < 2; i++) {
        unsigned long period = 1000000 / notes[i];
        unsigned long startNote = millis();
        unsigned long lastToggle = 0;
        bool state = true;
        
        while (millis() - startNote < (unsigned long)durations[i]) {
            unsigned long now = micros();
            if (now - lastToggle >= period/2) {
                state = !state;
                digitalWrite(ONE_WIRE_BUS, state);
                lastToggle = now;
            }
            yield();
        }
        
        digitalWrite(ONE_WIRE_BUS, HIGH);
        delay(50);
    }
    
    pinMode(ONE_WIRE_BUS, INPUT_PULLUP);
    digitalWrite(ONE_WIRE_BUS, HIGH);
    
    Serial.println("[PIR] ✅ Doorbell completed");
}

// ===============================================================
//                 ДОЗВОН ПРИ НИЗКОМ НАПРЯЖЕНИИ
// ===============================================================
void checkLowVoltageAndCall() {
    Serial.println("\n=== LOW VOLTAGE CHECK ===");
    
    // Используем isSim800Present()
    if (!cfg.gsmEnabled || !isSim800Present()) {
        Serial.println("[VOLT] GSM disabled or SIM800 not present - skipping call");
        return;
    }
    
    Serial.println("[VOLT] Measuring CHARGE voltage...");
    
    pinMode(PIN_CHARGE, OUTPUT);
    digitalWrite(PIN_CHARGE, HIGH);
    delay(5);
    
    ChannelResult charge = measureSingleChannel(PIN_CHARGE, "CHARGE");
    
    float chargeVoltage = charge.voltage;
    Serial.printf("[VOLT] CHARGE voltage: %.3f V\n", chargeVoltage);
    
    if (chargeVoltage < 2.0f) {
        Serial.println("[VOLT] ⚠️ CHARGE < 2.0V - initiating call sequence!");
        
        digitalWrite(GSM_POW_Ring_Zamok, HIGH);
        delay(100);
        
        if (!sim800Init()) {
            Serial.println("[VOLT] SIM800 init failed - cannot make calls");
            digitalWrite(GSM_POW_Ring_Zamok, LOW);
            return;
        }
        
        String callNumber = cfg.call;
        if (callNumber.length() == 0) {
            callNumber = CALL_NUMBER1;
        }
        
        if (callNumber.length() == 0) {
            Serial.println("[VOLT] No call number configured!");
            digitalWrite(GSM_POW_Ring_Zamok, LOW);
            return;
        }
        
        Serial.printf("[VOLT] Using call number: %s\n", callNumber.c_str());
        
        for (int attempt = 1; attempt <= 3; attempt++) {
            Serial.printf("[VOLT] Call attempt %d/3\n", attempt);
            
            String cmd = "ATD" + callNumber + ";";
            sim800Command(cmd, 2000);
            
            delay(5000);
            
            sim800Command("ATH", 1000);
            Serial.printf("[VOLT] Call %d ended\n", attempt);
            
            if (attempt < 3) {
                Serial.println("[VOLT] Waiting 15 seconds before next attempt...");
                delay(15000);
            }
        }
        
        digitalWrite(GSM_POW_Ring_Zamok, LOW);
        Serial.println("[VOLT] Call sequence completed, SIM800 powered off");
    } else {
        Serial.printf("[VOLT] CHARGE voltage OK (%.3fV >= 2.0V) - no call needed\n", chargeVoltage);
    }
    
    digitalWrite(PIN_CHARGE, HIGH);
    pinMode(PIN_CHARGE, INPUT_PULLUP);
    
    Serial.println("=== LOW VOLTAGE CHECK END ===\n");
}

// ===============================================================
//                ОБРАБОТКА ОБНОВЛЕНИЯ ПРОШИВКИ И RTC
// ===============================================================
void handleFirmwareAndRTC() {
    loadConfigWrapper();

    const uint32_t FW_ADDR = 0;
    EEPROM.begin(32);
    char oldVer[16] = {0};
    EEPROM.get(FW_ADDR, oldVer);

    bool newFW = (strcmp(oldVer, OTA_VERSION) != 0);

    // 1) Пробуем загрузить RTC как есть
    bool rtcOk = rtcMemLoad();   // <-- важно: именно rtcMemLoad()

    if (newFW) {
        Serial.printf("New firmware detected: %s -> %s\n", oldVer, OTA_VERSION);

        strcpy(oldVer, OTA_VERSION);
        EEPROM.put(FW_ADDR, oldVer);
        EEPROM.commit();

        Serial.println("Resetting RTC for new firmware...");
        rtcMemInit();

        setSim800Present(checkSim800Presence());
        rtcSave();

        // Сброс конфига — как при прошивке
        resetConfigToDefaults();
    }
    else if (!rtcOk) {
        // ✅ ПОЛНЫЙ OFF питания / сброс RTC memory: теперь делаем ТО ЖЕ, что при перепрошивке
        Serial.println("[RTC] RTC invalid after power loss/reset -> cold start init (SAME AS NEW FIRMWARE)");

        // Сбрасыва��м RTC
        rtcMemInit();

        // Обновляем флаг наличия SIM800
        setSim800Present(checkSim800Presence());
        rtcSave();

        // ✅ Сбрасываем конфиг как при перепрошивке
        resetConfigToDefaults();

        // (опционально) можно обновить oldVer в EEPROM, но обычно не нужно,
        // потому что прошивка не менялась — newFW=false.
    }
    // else: RTC валиден и прошивка та же — ничего не делаем, rtc уже загружен

    printRTCStatus();

    static uint8_t checkCounter = 0;
    if (!isSim800Present() && (++checkCounter % 100 == 0)) {
        Serial.printf("[SIM800] Periodic recheck (%d cycles)...\n", checkCounter);
        bool present = checkSim800Presence();
        if (present != isSim800Present()) {
            setSim800Present(present);
            rtcSave();
            Serial.printf("[SIM800] Presence flag updated to: %s\n", present ? "YES" : "NO");
        }
    }
}
// ===============================================================
//                    СТАНДАРТНЫЙ РЕЖИМ
// ===============================================================
void handlePIRInterrupt() {
    uint32_t start = millis();
    
    while (millis() - start < 3000) {
        if (digitalRead(PIR_AP_GND_SENS) == HIGH) {
            uint32_t t = millis() - start;
            if (t >= 2000) {
                ALARM();
                return;
            } else {
                rtc.wakeHour = 0;
                rtc.baseTimestamp = 0;
                rtc.dailySendCount = 0;
                rtcSave();
                enterAPMode(300);
                return;
            }
        }
        yield();
    }
    ALARM();
}
void handleStandardMode() {
    if (digitalRead(PIR_AP_GND_SENS) == 0) {
        handlePIRInterrupt();
        return;
    }

    // Первый запуск после прошивки/сброса RTC:
    // хотим измерить Hour 0 и на следующий цикл перейти к Hour 1
    bool first = (rtc.wakeHour == 0 && rtc.baseTimestamp == 0 && rtc.dailySendCount == 0);

    if (first) {
        // Hour 0
        measureAndSaveHour();

        // Следующий цикл будет Hour 1
        rtc.wakeHour = 1;
        rtcSave();

        Serial.printf("[SLEEP] Going to sleep for %d seconds\n", cfg.sleepSec);
        Serial.flush();
        ESP.deepSleep(cfg.sleepSec * 1000000ULL);
        return;
    }

    // ВАЖНО: НЕ увеличиваем wakeHour ДО измерения, иначе пропускается Hour 1
    if (rtc.wakeHour >= 24) {
        dailyCycle();
        return;
    } else {
        hourlyCycle();   // hourlyCycle() измерит текущий rtc.wakeHour и САМ увеличит его в конце
        return;
    }
}// ===============================================================
//                    ФУНКЦИИ УПРАВЛЕНИЯ ПИНАМИ
// ===============================================================
void initializePins() {
    Serial.println("Initializing pins...");
    pinMode(PIR_AP_GND_SENS, INPUT_PULLUP);
    pinMode(SOL_GSM_RX, OUTPUT); digitalWrite(SOL_GSM_RX, HIGH);
    pinMode(ACC_GSM_TX, OUTPUT); digitalWrite(ACC_GSM_TX, HIGH);
    pinMode(PIN_CHARGE, OUTPUT); digitalWrite(PIN_CHARGE, HIGH);
    pinMode(GSM_POW_Ring_Zamok, OUTPUT); digitalWrite(GSM_POW_Ring_Zamok, LOW);
}

// ===============================================================
//                           SETUP
// ===============================================================
void setup() {
    Serial.begin(115200);
    
    printBootInfo();
    
    initializePins();
    
    handleFirmwareAndRTC();
    // Автоматический сброс тары только при первом запуске
if (rtc.reserved3 == 0) {
   doOneTimeTare();
}
    bool firstCycle = (rtc.reserved3 == 0);
    
    if (cfg.gsmEnabled) {
        sim800.begin(9600);
        Serial.println("GSM enabled - SIM800 initialized");
    } else {
        Serial.println("GSM disabled in config - SIM800 initialization skipped");
    }
    
    handleTime(0, 0, 0, 0, 0);
/*   
    if (firstCycle) {
        Serial.println("SYSTEM: Первый цикл - быстрый сон без проверокOTA");
        rtc.reserved3 = 1;
        rtcSave();
        Serial.flush();
        ESP.deepSleep(cfg.sleepSec * 1000000ULL, WAKE_RF_DEFAULT);
        return;
    }
 */    
#if VIESSMANN_ENABLED
    checkLowVoltageAndCall();
    handleViessmannMode();
#elif DOMOFON_ENABLED
    checkLowVoltageAndCall();
    handleDomofonMode();
#else
    handleStandardMode();
#endif
}

void loop() {}