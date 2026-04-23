// BUILD_TIMESTAMP: 2026-04-04 21:54:41
// viessmann.cpp — Полная логика режима Viessmann с универсальной функцией времени
// ИСПРАВЛЕНО: OTA работает, SIM800 корректно перезагружается при ошибках

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <FastBot2.h>
#include <time.h>
#include "config.h"
#include "rtc.h"
#include <SoftwareSerial.h>

// ===============================================================
//                    ОПРЕДЕЛЕНИЕ СТРУКТУРЫ TimeData
// ===============================================================
struct TimeData {
    int hour;
    int min;
    int wday;
    bool inPeriod;
};

// ===============================================================
//                    ВНЕШНИЕ ГЛОБАЛЬНЫЕ ОБЪЕКТЫ
// ===============================================================
extern RTCStore rtc;
extern ConfigData cfg;
extern FastBot2 bot;
extern FastBot2 bot2;
extern String wifiScanBeforeConnect;

// ВНЕШНИЕ ПЕРЕМЕННЫЕ ИЗ ДРУГИХ ФАЙЛОВ
extern SoftwareSerial sim800;

// ===============================================================
//                    ВНЕШНИЕ ФУНКЦИИ
// ===============================================================
extern bool wifi_connect(String& outBestSSID, int& outBestRSSI, int& outBestChannel, 
                         uint8_t outBestBSSID[6], String& scanLog);
extern void measureAndSaveHour();
extern void OTA();
extern bool sim800Init();
extern bool sim800SendSMS(const String& phone, const String& msg);
extern bool checkSim800Presence();
extern void rtcSave();
extern void sim800Debug();

// ===============================================================
//                    СТАТИЧЕСКИЕ ПЕРЕМЕННЫЕ ДЛЯ ВРЕМЕНИ
// ===============================================================
static uint32_t bootMinutes = 0;
static int estHour = 0, estMin = 0, estWday = 1;
static uint32_t lastSim800Attempt = 0;
static uint8_t sim800FailCount = 0;

// ===============================================================
//                    УНИВЕРСАЛЬНАЯ ФУНКЦИЯ ВРЕМЕНИ
// ===============================================================
TimeData handleTime(int mode, int startHour, int startMin, int endHour, int endMin) {
    
    TimeData result = {0, 0, 1, false};
    
    // ========== РЕЖИМ 0: ИНИЦИАЛИЗАЦИЯ ==========
    if (mode == 0) {
        if (rtc.baseTimestamp > 1700000000UL) {
            time_t t = rtc.baseTimestamp;
            struct tm* tm = gmtime(&t);
            estHour = tm->tm_hour;
            estMin = tm->tm_min;
            estWday = tm->tm_wday;
            Serial.printf("Initial time from RTC: %02d:%02d day %d\n", estHour, estMin, estWday);
        } else {
            estHour = 12;
            estMin = 0;
            estWday = 1;
            Serial.println("Initial estimated time: 12:00");
        }
        bootMinutes = 0;
        result.hour = estHour;
        result.min = estMin;
        result.wday = estWday;
    }
    
    // ========== РЕЖИМ 1: ОБНОВЛЕНИЕ ==========
    else if (mode == 1) {
        bootMinutes++;
        uint32_t total = (estHour * 60 + estMin + bootMinutes) % (24*60);
        estHour = total / 60;
        estMin = total % 60;
        if (bootMinutes % (24*60) == 0) {
            estWday = (estWday % 7) + 1;
        }
        
        // Если есть точное время в RTC, используем его
        if (rtc.baseTimestamp > 1700000000UL) {
            time_t t = rtc.baseTimestamp + (bootMinutes * 60);
            struct tm* tm = gmtime(&t);
            result.hour = tm->tm_hour;
            result.min = tm->tm_min;
            result.wday = tm->tm_wday;
        } else {
            result.hour = estHour;
            result.min = estMin;
            result.wday = estWday;
        }
    }
    
    // ========== РЕЖИМ 2: ПРОВЕРКА ПЕРИОДА ==========
    else if (mode == 2) {
        // Получаем текущее время
        if (rtc.baseTimestamp > 1700000000UL) {
            time_t t = rtc.baseTimestamp + (bootMinutes * 60);
            struct tm* tm = gmtime(&t);
            result.hour = tm->tm_hour;
            result.min = tm->tm_min;
            result.wday = tm->tm_wday;
        } else {
            result.hour = estHour;
            result.min = estMin;
            result.wday = estWday;
        }
        
        // Проверяем попадание в период
        int cur = result.hour * 60 + result.min;
        int start = startHour * 60 + startMin;
        int end = endHour * 60 + endMin;
        
        if (start <= end) {
            result.inPeriod = (cur >= start && cur < end);
        } else {
            result.inPeriod = (cur >= start || cur < end);
        }
    }
    
    return result;
}

void pulse(int pin, int ms) {
    pinMode(pin, OUTPUT); 
    digitalWrite(pin, LOW); 
    delay(ms);
    digitalWrite(pin, HIGH); 
    pinMode(pin, INPUT_PULLUP);
}

// ===============================================================
//                    ИНИЦИАЛИЗАЦИЯ SIM800 С ПОВТОРАМИ
// ===============================================================
bool initSim800WithRetry() {
    // Если слишком много ошибок подряд, делаем паузу
    if (sim800FailCount >= 3) {
        if (millis() - lastSim800Attempt < 60000) { // 1 минута паузы
            Serial.println("[SIM800] Too many failures, waiting...");
            return false;
        } else {
            sim800FailCount = 0; // Сбрасываем счетчик после паузы
        }
    }
    
    lastSim800Attempt = millis();
    
    // Полный сброс питания SIM800
    digitalWrite(GSM_POW_Ring_Zamok, LOW);
    delay(200);
    digitalWrite(GSM_POW_Ring_Zamok, HIGH);
    delay(500);
    
    if (sim800Init()) {
        sim800FailCount = 0;
        return true;
    } else {
        sim800FailCount++;
        return false;
    }
}

// ===============================================================
//                    ФУНКЦИЯ ПРОВЕРКИ OTA
// ===============================================================
void checkOTA() {
    // Проверяем, нужно ли делать OTA в этом цикле
    bool needOta = false;
    
    if (OTA_CHECK_INTERVAL == 1) {
        needOta = true;
    } else {
        // Используем dailySendCount для проверки
        // OTA нужна когда dailySendCount кратен OTA_CHECK_INTERVAL
        if ((rtc.dailySendCount % OTA_CHECK_INTERVAL) == 0) {
            needOta = true;
            Serial.printf("[VIESSMANN] dailySendCount=%u кратен %u - выполняем OTA\n", 
                         rtc.dailySendCount, OTA_CHECK_INTERVAL);
        }
    }
    
    Serial.printf("[VIESSMANN] dailySendCount = %u, needOta=%s\n", 
                  rtc.dailySendCount, needOta ? "YES" : "NO");
    
    if (needOta) {
        Serial.println("[VIESSMANN] Time for OTA check - connecting to WiFi...");
        
        String bestSSID, scanLog;
        int bestRSSI, bestChannel;
        uint8_t bestBSSID[6];
        
        bool wifiConnected = wifi_connect(bestSSID, bestRSSI, bestChannel, bestBSSID, scanLog);
        wifiScanBeforeConnect = scanLog;
        
        if (wifiConnected) {
            Serial.printf("[VIESSMANN] WiFi connected to %s\n", bestSSID.c_str());
            OTA();
            WiFi.disconnect();
            WiFi.mode(WIFI_OFF);
        } else {
            Serial.println("[VIESSMANN] No WiFi connection - skipping OTA");
        }
    }
}

// ===============================================================
//                    ОСНОВНАЯ ЛОГИКА VIESSMANN
// ===============================================================
// В функции check_F5():
void check_F5() {
    Serial.println("\n=== F5 CHECK START ===");
    
    // Получаем текущее время
    TimeData current = handleTime(1, 0, 0, 0, 0);
    
    int pirState = digitalRead(PIR_AP_GND_SENS);
    if (pirState != LOW) {
        Serial.println("[F5] PIR is NOT LOW - EXIT");
        return;
    }
    
    measureAndSaveHour();
    
    int currentHour = rtc.wakeHour;
    // Используем новые поля структуры
float solarVoltage = rtcHour(currentHour).solar50 / 50.0f;
float ds18b20Temp  = rtcHour(currentHour).t_ds5 / 2.0f;
    float threshold = VIESSMANN_REG;
    
    if (solarVoltage >= threshold) {
        Serial.println("[F5] SOLAR >= threshold - EXIT");
        return;
    }
    
    Serial.println("[F5] SOLAR < threshold - TRIGGERING ACTION!");
    pulse(ACC_GSM_TX, 1000);
    
    String bestSSID, scanLog;
    int bestRSSI, bestChannel;
    uint8_t bestBSSID[6];
    
    bool connected = wifi_connect(bestSSID, bestRSSI, bestChannel, bestBSSID, scanLog);
    wifiScanBeforeConnect = scanLog;
    
    String msg = "🚨 F5 ALERT\n";
    msg += "🌡️ DS18B20: " + String(ds18b20Temp, 1) + "°C\n";
    msg += "☀️ Solar: " + String(solarVoltage, 2) + "V\n";
    msg += "⚡ Threshold: " + String(threshold, 2) + "V\n";
    msg += "⏰ Time: " + String(current.hour) + ":" + (current.min < 10 ? "0" : "") + String(current.min) + "\n";
    
    if (connected) {
        msg += "📶 Connected to: " + bestSSID + " (" + String(bestRSSI) + " dBm)\n";
        
        fb::Message m;
        m.chatID = BOT2_CHATID;
        m.text = msg;
        bot2.sendMessage(m);
        
        WiFi.disconnect();
        WiFi.mode(WIFI_OFF);
    } 
    // Отправка SMS только если GSM включен - используем isSim800Present()
    else if (cfg.gsmEnabled && isSim800Present()) {
        if (initSim800WithRetry()) {
            String smsMsg = "F5 ALERT Solar:" + String(solarVoltage,2) + 
                           "V Temp:" + String(ds18b20Temp,1) + "C " +
                           "Time:" + String(current.hour) + ":" + String(current.min);
            sim800SendSMS(cfg.sms, smsMsg);
        } else {
            Serial.println("[F5] SIM800 init failed - SMS skipped");
        }
    } else if (cfg.gsmEnabled && !isSim800Present()) {
        Serial.println("[F5] SIM800 not present - SMS skipped");
    } else {
        Serial.println("[F5] GSM disabled - SMS skipped");
    }
    
    Serial.println("=== F5 CHECK END ===\n");
}
// ===============================================================
//                    ПОЛНАЯ ФУНКЦИЯ VIESSMANN MODE
// ===============================================================
void handleViessmannMode() {
    Serial.println("\n=== VIESSMANN MODE START ===");
    
    // Увеличиваем счетчик циклов для OTA
    rtc.dailySendCount++;
    rtcSave();
    Serial.printf("[VIESSMANN] dailySendCount increased to %u\n", rtc.dailySendCount);
    
    // Обновляем время
    TimeData current = handleTime(1, 0, 0, 0, 0);
    Serial.printf("[TIME] Current: %02d:%02d Day %d\n", current.hour, current.min, current.wday);
    
    // Включаем питание SIM800 если нужно
    if (cfg.gsmEnabled && isSim800Present()) {
        digitalWrite(GSM_POW_Ring_Zamok, HIGH);
        Serial.println("[SIM800] Power ON for Viessmann mode");
        delay(100); // Даем время на включение
    } else if (cfg.gsmEnabled && !isSim800Present()) {
        Serial.println("[SIM800] SIM800 not present - power not enabled");
    } else {
        Serial.println("[SIM800] GSM disabled in config");
    }
    
    // ===========================================================
    //                    ПРОВЕРКА PIR
    // ===========================================================
    int pirState = digitalRead(PIR_AP_GND_SENS);
    Serial.printf("[PIR] Current state: %d (%s)\n", pirState, pirState == LOW ? "ACTIVE" : "INACTIVE");
    
    if (pirState == LOW) {
        Serial.println("[PIR] ⚠️ PIR is LOW - executing pulse and sound!");
        
        // СНАЧАЛА генерируем звуковой сигнал
        extern void RingTone();
        RingTone();
        
        // ПОТОМ выполняем pulse
        Serial.println("[PIR] Executing 10 second pulse on SOL_GSM_RX");
        pulse(SOL_GSM_RX, 10000);
        Serial.println("[PIR] Pulse completed");
    }
    
    // ===========================================================
    //              ПРОВЕРКА ПЕРИОДОВ ON/OFF
    // ===========================================================
    uint8_t counter = rtc.rehabHours;
    
    // Проверяем периоды
    bool inOnPeriod = handleTime(2, VIESSMANN_ON_START_HOUR, VIESSMANN_ON_START_MIN,
                                   VIESSMANN_ON_END_HOUR, VIESSMANN_ON_END_MIN).inPeriod;
    
    bool inOffPeriod = handleTime(2, VIESSMANN_OFF_START_HOUR, VIESSMANN_OFF_START_MIN,
                                    VIESSMANN_OFF_END_HOUR, VIESSMANN_OFF_END_MIN).inPeriod;
    
    Serial.printf("[PERIODS] inOnPeriod=%s, inOffPeriod=%s\n", 
                  inOnPeriod ? "YES" : "NO", inOffPeriod ? "YES" : "NO");
    Serial.printf("[FLAGS] onExecuted=%s, offExecuted=%s\n",
                  isOnExecuted() ? "YES" : "NO", isOffExecuted() ? "YES" : "NO");
    
    // Сброс флагов в полночь
    static int lastResetDay = -1;
    int currentDay = bootMinutes / (24*60);
    if (lastResetDay != currentDay && current.hour == 0 && current.min == 0) {
        Serial.println("[PERIODS] Midnight reset - clearing ON/OFF flags");
        setOnExecuted(false);
        setOffExecuted(false);
        lastResetDay = currentDay;
        rtcSave();
    }
    
    // ===========================================================
    //              ВЫПОЛНЕНИЕ ON ПЕРИОДА
    // ===========================================================
    if (inOnPeriod && !isOnExecuted()) {
        Serial.println("[VIESSMANN] 🔵 ON period - executing pulses with sound");
        
        // Генерируем звук
        extern void RingTone();
        RingTone();
        
        // Выполняем脉冲ы
        Serial.println("[VIESSMANN] Pulse 1: SOL_GSM_RX (3500ms)");
        pulse(SOL_GSM_RX, 3500);
        
        Serial.println("[VIESSMANN] Pulse 2: PIN_CHARGE (3500ms)");
        pulse(PIN_CHARGE, 3500);
        
        // Устанавливаем флаг
        setOnExecuted(true);
        rtcSave();
        Serial.println("[VIESSMANN] ✅ ON period completed, flag set");
    }
    
    // ===========================================================
    //              ВЫПОЛНЕНИЕ OFF ПЕРИОДА
    // ===========================================================
    if (inOffPeriod && !isOffExecuted()) {
        Serial.println("[VIESSMANN] 🔴 OFF period - executing pulse with sound");
        
        // Генерируем звук
        extern void RingTone();
        RingTone();
        
        // Выполняем脉冲
        Serial.println("[VIESSMANN] Pulse: SOL_GSM_RX (10000ms)");
        pulse(SOL_GSM_RX, 10000);
        
        // Устанавливаем флаг
        setOffExecuted(true);
        rtcSave();
        Serial.println("[VIESSMANN] ✅ OFF period completed, flag set");
    }
    
    // ===========================================================
    //              СЧЕТЧИК И ОТПРАВКА ОТЧЕТОВ
    // ===========================================================
    counter++;
    Serial.printf("[COUNTER] rehabHours: %d -> %d\n", rtc.rehabHours, counter);
    
    if (counter >= 12) {
        counter = 0;
        Serial.println("[REPORT] Counter reached 12 - sending report");
        
        String bestSSID, scanLog;
        int bestRSSI, bestChannel;
        uint8_t bestBSSID[6];
        
        Serial.println("[WiFi] Attempting to connect for report...");
        bool wifiConnected = wifi_connect(bestSSID, bestRSSI, bestChannel, bestBSSID, scanLog);
        wifiScanBeforeConnect = scanLog;
        
        // Измеряем данные для текущего часа
        measureAndSaveHour();
        
        if (wifiConnected) {
            Serial.printf("[WiFi] Connected to %s (RSSI: %d dBm)\n", bestSSID.c_str(), bestRSSI);
            
            auto& h = rtcHour(rtc.wakeHour);
            
            // Конвертируем из сжатых форматов
            float t_ds = h.t_ds5 / 2.0f;
            float t_bme = h.t_bme5 / 2.0f;
            float hum = (float)h.hum5;

            float press = 900.0f + h.pressDelta;
            float acc = h.acc50 / 50.0f;
            float sol = h.solar50 / 50.0f;
            
            String period = inOnPeriod ? "ON" : (inOffPeriod ? "OFF" : "IDLE");
            String msg = "📊[" + period + "] " + String(current.hour) + ":" + 
                        (current.min < 10 ? "0" : "") + String(current.min) +
                         "\n🌡️" + String(t_ds,1) + "°C" +
                         "\n🌡️BME" + String(t_bme,1) + "°C" +
                         "\n💧" + String(hum,0) + "%" +
                         "\n📊" + String(press,0) + "hPa" +
                         "\n🔋" + String(acc,2) + "V" +
                         "\n☀️" + String(sol,2) + "V";
            
            Serial.println("[Telegram] Sending message...");
            fb::Message m;
            m.chatID = CHAT_ID;
            m.text = msg;
            bot.sendMessage(m);
            
            WiFi.disconnect();
            WiFi.mode(WIFI_OFF);
            Serial.println("[WiFi] Disconnected");
        }
        // Отправка SMS если нет WiFi
        else if (cfg.gsmEnabled && isSim800Present()) {
            Serial.println("[SIM800] No WiFi, sending SMS via GSM...");
            
            if (initSim800WithRetry()) {
                auto& h = rtcHour(rtc.wakeHour);
                
                // Конвертируем из сжатых форматов
                float t_ds = h.t_ds5 / 2.0f;
                float acc = h.acc50 / 50.0f;
                
                String smsMsg = "Hour " + String(rtc.wakeHour) + 
                               " T:" + String(t_ds,1) + "C" +
                               " Bat:" + String(acc,2) + "V" +
                               " Time:" + String(current.hour) + ":" + 
                               (current.min < 10 ? "0" : "") + String(current.min);
                
                Serial.printf("[SMS] Sending: %s\n", smsMsg.c_str());
                sim800SendSMS(cfg.sms, smsMsg);
            } else {
                Serial.println("[SIM800] Init failed - SMS skipped");
            }
        } else if (cfg.gsmEnabled && !isSim800Present()) {
            Serial.println("[SIM800] SIM800 not present - SMS skipped");
        } else {
            Serial.println("[SMS] GSM disabled - SMS skipped");
        }
    }
    
    // Сохраняем обновленный счетчик
    rtc.rehabHours = counter;
    rtcSave();
    
    // ===========================================================
    //              ВЫКЛЮЧАЕМ SIM800
    // ===========================================================
    if (cfg.gsmEnabled && isSim800Present()) {
        digitalWrite(GSM_POW_Ring_Zamok, LOW);
        Serial.println("[SIM800] Power OFF before sleep");
    }
    
    // ===========================================================
    //              ПРОВЕРКА OTA
    // ===========================================================
    Serial.println("[OTA] Checking if OTA needed...");
    checkOTA();
    
    // ===========================================================
    //              ПЕРЕХОД В СОН
    // ===========================================================
    Serial.printf("[SLEEP] Going to sleep for %d seconds\n", SLEEP_SEC);
    Serial.println("=== VIESSMANN MODE END ===\n");
    Serial.flush();
    
    ESP.deepSleep(SLEEP_SEC * 1000000ULL);
}