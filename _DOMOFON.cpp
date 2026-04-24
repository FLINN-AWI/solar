// BUILD_TIMESTAMP: 2026-04-23 23:18:12
// FILE_BUILD_TIMESTAMP: 2026-04-23 12:51:18
// _DOMOFON.cpp — Режим домофона: открытие двери по звонку с разрешённого номера
// ОПТИМИЗИРОВАННАЯ ВЕРСИЯ - ускорение цикла
// Добавлен сброс чужих вызовов
// Исправлен счетчик dailySendCount для OTA

#include <Arduino.h>
#include <SoftwareSerial.h>
#include "config.h"
#include "rtc.h"

// ===============================================================
//                    ВНЕШНИЕ ГЛОБАЛЬНЫЕ ОБЪЕКТЫ
// ===============================================================
extern RTCStore rtc;
extern ConfigData cfg;
extern SoftwareSerial sim800;

// ===============================================================
//                    ВНЕШНИЕ ФУНКЦИИ
// ===============================================================
extern bool sim800Init();
extern String sim800Command(const String& cmd, uint32_t timeout);
extern void rtcSave();
extern void OTA();
extern bool wifi_connect(String& outBestSSID, int& outBestRSSI, int& outBestChannel, 
                         uint8_t outBestBSSID[6], String& scanLog);
extern String wifiScanBeforeConnect;

// ===============================================================
//                    СТАТИЧЕСКИЕ ПЕРЕМЕННЫЕ
// ===============================================================
static String incomingNumber = "";
static uint32_t lastWakeTime = 0;  // Для измерения времени цикла

// ===============================================================
//                    ФУНКЦИЯ ПУЛЬСА ДЛЯ ОТКРЫТИЯ ДВЕРИ
// ===============================================================
void doorOpenPulse() {
    Serial.println("[DOMOFON] 🚪 Opening door with pulse sequence...");
    
    pinMode(GSM_POW_Ring_Zamok, OUTPUT);
    
    // Уменьшена задержка между импульсами
    for (int i = 0; i < 3; i++) {
        digitalWrite(GSM_POW_Ring_Zamok, HIGH);
        delay(5);  // Было 10
        digitalWrite(GSM_POW_Ring_Zamok, LOW);
        delay(10); // Было 20
        Serial.printf("[DOMOFON] Pulse %d/3 completed\n", i + 1);
    }
    
    digitalWrite(GSM_POW_Ring_Zamok, HIGH);
    Serial.println("[DOMOFON] SIM800 power ON");
    delay(2);  // Было 5
    
    Serial.println("[DOMOFON] ✅ Door opening sequence completed");
}

// ===============================================================
//                    ПРОВЕРКА PIR И ОТКРЫТИЕ ДВЕРИ
// ===============================================================
void checkPirAndOpenDoor() {
    int pirState = digitalRead(PIR_AP_GND_SENS);
    
    if (pirState == LOW) {
        Serial.println("[DOMOFON] 🔔 PIR is LOW - opening door!");
        doorOpenPulse();
        
        // После открытия двери сразу уходим в сон, без OTA
        Serial.println("[DOMOFON] PIR triggered - going to sleep");
        Serial.flush();
        ESP.deepSleep(cfg.sleepSec * 1000000ULL, WAKE_RF_DEFAULT);
    }
}

// ===============================================================
//                    ПРОВЕРКА РАЗРЕШЁННОГО НОМЕРА
// ===============================================================
bool isAllowedNumber(const String& number) {
    String cleanNumber = number;
    cleanNumber.replace(" ", "");
    cleanNumber.replace("+", "");
    cleanNumber.replace("-", "");
    cleanNumber.replace("(", "");
    cleanNumber.replace(")", "");
    
    Serial.printf("[DOMOFON] Checking number: '%s' (cleaned: '%s')\n", 
                  number.c_str(), cleanNumber.c_str());
    
    if (strlen(CALL_NUMBER1) > 0) {
        String allowed1 = String(CALL_NUMBER1);
        allowed1.replace("+", "");
        if (cleanNumber == allowed1 || cleanNumber.indexOf(allowed1) != -1) {
            Serial.println("[DOMOFON] ✅ Matched CALL_NUMBER1");
            return true;
        }
    }
    
    if (strlen(CALL_NUMBER2) > 0) {
        String allowed2 = String(CALL_NUMBER2);
        allowed2.replace("+", "");
        if (cleanNumber == allowed2 || cleanNumber.indexOf(allowed2) != -1) {
            Serial.println("[DOMOFON] ✅ Matched CALL_NUMBER2");
            return true;
        }
    }
    
    if (strlen(CALL_NUMBER3) > 0) {
        String allowed3 = String(CALL_NUMBER3);
        allowed3.replace("+", "");
        if (cleanNumber == allowed3 || cleanNumber.indexOf(allowed3) != -1) {
            Serial.println("[DOMOFON] ✅ Matched CALL_NUMBER3");
            return true;
        }
    }
    
    if (cfg.call.length() > 0) {
        String cfgNumber = cfg.call;
        cfgNumber.replace("+", "");
        if (cleanNumber == cfgNumber || cleanNumber.indexOf(cfgNumber) != -1) {
            Serial.println("[DOMOFON] ✅ Matched cfg.call");
            return true;
        }
    }
    
    Serial.println("[DOMOFON] ❌ Number not in allowed list");
    return false;
}

// ===============================================================
//                    ПАРСИНГ ВХОДЯЩЕГО НОМЕРА ИЗ RING
// ===============================================================
String extractNumberFromRing(const String& ringLine) {
    int start = ringLine.indexOf('"');
    if (start == -1) return "";
    
    int end = ringLine.indexOf('"', start + 1);
    if (end == -1) return "";
    
    return ringLine.substring(start + 1, end);
}

// ===============================================================
//                    БЫСТРАЯ ПРОВЕРКА ВХОДЯЩИХ ЗВОНКОВ
// ===============================================================
bool checkIncomingCall() {
    // Очищаем буфер
    while (sim800.available()) sim800.read();
    
    // Быстрая проверка через AT+CLCC (без лишних задержек)
    sim800.println("AT+CLCC");
    
    unsigned long startTime = millis();
    String response = "";
    bool callProcessed = false;
    
    // Ждем максимум 500 мс
    while (millis() - startTime < 500) {
        while (sim800.available()) {
            String line = sim800.readStringUntil('\n');
            line.trim();
            
            if (line.length() > 0) {
                if (line.indexOf("+CLCC:") != -1) {
                    int numStart = line.indexOf("\"");
                    if (numStart != -1) {
                        int numEnd = line.indexOf("\"", numStart + 1);
                        if (numEnd != -1) {
                            incomingNumber = line.substring(numStart + 1, numEnd);
                            Serial.printf("[DOMOFON] Active call from: %s\n", incomingNumber.c_str());
                            
                            if (isAllowedNumber(incomingNumber)) {
                                Serial.println("[DOMOFON] ✅ Allowed number - opening door!");
                                sim800Command("ATH", 200); // Сбрасываем вызов
                                callProcessed = true;
                            } else {
                                Serial.println("[DOMOFON] ❌ Unknown number - rejecting call");
                                sim800Command("ATH", 200); // Сбрасываем чужой вызов
                                // Не возвращаем true, чтобы не открывать дверь
                            }
                        }
                    }
                }
            }
        }
        delay(10);
    }
    
    // Если не было активного звонка, проверяем буфер на наличие RING
    if (!callProcessed) {
        // Очищаем буфер и проверяем еще раз
        while (sim800.available()) sim800.read();
        sim800.println("AT+CLCC");
        delay(100);
        
        while (sim800.available()) {
            String line = sim800.readStringUntil('\n');
            line.trim();
            if (line.indexOf("+CLCC:") != -1) {
                int numStart = line.indexOf("\"");
                if (numStart != -1) {
                    int numEnd = line.indexOf("\"", numStart + 1);
                    if (numEnd != -1) {
                        incomingNumber = line.substring(numStart + 1, numEnd);
                        Serial.printf("[DOMOFON] Call from: %s\n", incomingNumber.c_str());
                        
                        if (isAllowedNumber(incomingNumber)) {
                            Serial.println("[DOMOFON] ✅ Allowed number - opening door!");
                            sim800Command("ATH", 200);
                            callProcessed = true;
                        } else {
                            Serial.println("[DOMOFON] ❌ Unknown number - rejecting call");
                            sim800Command("ATH", 200); // Сбрасываем чужой вызов
                        }
                    }
                }
            }
        }
    }
    
    return callProcessed; // Возвращаем true только если был разрешенный номер
}

// ===============================================================
//                    ФУНКЦИЯ ПЕРЕХОДА В ГЛУБОКИЙ СОН С OTA
// ===============================================================
void goToDeepSleepWithOTA() {
    uint32_t cycleTime = millis() - lastWakeTime;
    Serial.printf("[DOMOFON] Cycle completed in %lu ms\n", (unsigned long)cycleTime);
    Serial.printf("[DOMOFON] Preparing for deep sleep (%u seconds)\n", cfg.sleepSec);
    // Проверяем, нужно ли делать OTA в этом цикле
    bool needOta = false;
    
    // Проверка по интервалу
    if (OTA_CHECK_INTERVAL == 1) {
        needOta = true;
    } else {
        // Используем dailySendCount для проверки
        // OTA нужна когда dailySendCount кратен OTA_CHECK_INTERVAL
        if ((rtc.dailySendCount % OTA_CHECK_INTERVAL) == 0) {
            needOta = true;
        }
    }
    
    Serial.printf("[DOMOFON] dailySendCount = %u, needOta=%s\n", 
                  rtc.dailySendCount, needOta ? "YES" : "NO");
    
    if (needOta) {
        Serial.println("[DOMOFON] Time for OTA check - connecting to WiFi...");
        
        // Быстрое подключение к WiFi
        String bestSSID, scanLog;
        int bestRSSI, bestChannel;
        uint8_t bestBSSID[6];
        
        bool wifiConnected = wifi_connect(bestSSID, bestRSSI, bestChannel, bestBSSID, scanLog);
        wifiScanBeforeConnect = scanLog;
        
if (wifiConnected) {
    Serial.printf("[DOMOFON] WiFi connected to %s (%lu ms)\n", 
                  bestSSID.c_str(), millis() - lastWakeTime - cycleTime);
    OTA();
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
}
else {
            Serial.println("[DOMOFON] No WiFi connection - skipping OTA");
        }
    } else {
        Serial.println("[DOMOFON] No OTA needed this cycle");
    }
    
    Serial.printf("[DOMOFON] Total cycle time: %lu ms\n", millis() - lastWakeTime);  // %u -> %lu
    Serial.println("[DOMOFON] Going to deep sleep now");
    Serial.flush();
    ESP.deepSleep(cfg.sleepSec * 1000000ULL, WAKE_RF_DEFAULT);
}

// ===============================================================
//                    ОСНОВНАЯ ФУНКЦИЯ РЕЖИМА
// ===============================================================
// ===============================================================
//                    ОСНОВНАЯ ФУНКЦИЯ РЕЖИМА
// ===============================================================
void handleDomofonMode() {
    lastWakeTime = millis();
    Serial.println("\n=== DOMOFON MODE START ===");
    
    // Увеличиваем счетчик циклов для OTA
    // Первый цикл после прошивки обрабатывается в main.cpp и сюда не попадает
    rtc.dailySendCount++;
    rtcSave();
    Serial.printf("[DOMOFON] dailySendCount increased to %u\n", rtc.dailySendCount);
    
    // ПРОВЕРЯЕМ PIR, НО НЕ УХОДИМ СРАЗУ В СОН
    bool pirTriggered = (digitalRead(PIR_AP_GND_SENS) == LOW);
    
    if (pirTriggered) {
        Serial.println("[DOMOFON] 🔔 PIR is LOW!");
        
        // СНАЧАЛА генерируем звуковой сигнал на ONE_WIRE_BUS
        extern void RingTone();
        RingTone();
        
        // ПОТОМ открываем дверь
        doorOpenPulse();
        
        // После открытия двери сразу уходим в сон, без OTA
        Serial.println("[DOMOFON] PIR triggered - going to sleep");
        Serial.flush();
        ESP.deepSleep(cfg.sleepSec * 1000000ULL, WAKE_RF_DEFAULT);
        return;  // Важно: возвращаемся, чтобы не выполнять остальной код
    }
    
    // Если GSM отключен - сразу в сон
    if (!cfg.gsmEnabled) {
        Serial.println("[DOMOFON] GSM disabled - going to sleep");
        goToDeepSleepWithOTA();
        return;
    }
    
    // Быстрая инициализация SIM800
    Serial.println("[DOMOFON] Initializing SIM800...");
    
    pinMode(ACC_GSM_TX, OUTPUT);
    pinMode(SOL_GSM_RX, INPUT);
    
    // Очищаем буфер
    while (sim800.available()) sim800.read();
    
    // Пытаемся инициализировать SIM800 (с таймаутом)
    unsigned long initStart = millis();
    bool simOk = false;
    
    // Даем SIM800 меньше времени на пробуждение
    delay(100);
    
    if (sim800Init()) {
        simOk = true;
        Serial.printf("[DOMOFON] SIM800 initialized in %lu ms\n", millis() - initStart);
    } else {
        Serial.printf("[DOMOFON] SIM800 init FAILED in %lu ms\n", millis() - initStart);
    }
    
    if (simOk) {
        // Быстрая проверка входящих звонков
        if (checkIncomingCall()) {
            Serial.println("[DOMOFON] ✅ Allowed number detected - opening door!");
            
            // Генерируем звук при открытии двери по звонку
            extern void RingTone();
            RingTone();
            
            doorOpenPulse();
        } else {
            Serial.println("[DOMOFON] No allowed calls detected");
        }
    }
    
    // Финальная проверка PIR (на случай, если PIR сработал во время работы с SIM800)
    if (digitalRead(PIR_AP_GND_SENS) == LOW) {
        Serial.println("[DOMOFON] 🔔 PIR triggered in final check!");
        
        // Генерируем звук
        extern void RingTone();
        RingTone();
        
        doorOpenPulse();
        
        Serial.println("[DOMOFON] PIR triggered - going to sleep");
        Serial.flush();
        ESP.deepSleep(cfg.sleepSec * 1000000ULL, WAKE_RF_DEFAULT);
        return;
    }
    
    // Переход в сон с OTA
    goToDeepSleepWithOTA();
}