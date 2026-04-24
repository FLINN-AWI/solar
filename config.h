// BUILD_TIMESTAMP: 2026-04-23 23:18:12
// FILE_BUILD_TIMESTAMP: 2026-04-23 12:51:18
// config.h - Project configuration and pinout


#pragma once
#include <Arduino.h>
#include <AutoOTA.h>

extern void saveConfig();

// HX711 persistent tare (LittleFS)
extern bool loadHx711Tare();
extern bool saveHx711Tare(long offset);
extern void clearHx711Tare();
extern bool hasHx711Tare();
extern long hx711TareOffset();
extern bool ensureHx711TareSaved();
// ===================== AUTO VERSION FROM BUILD TIME =====================

// Преобразование месяца "Jan", "Feb", ... в число 01-12
constexpr int BUILD_MONTH() {
    return (__DATE__[0] == 'J' && __DATE__[1] == 'a') ? 1 :  // Jan
           (__DATE__[0] == 'F') ? 2 :                         // Feb
           (__DATE__[0] == 'M' && __DATE__[2] == 'r') ? 3 :   // Mar
           (__DATE__[0] == 'A' && __DATE__[1] == 'p') ? 4 :   // Apr
           (__DATE__[0] == 'M' && __DATE__[2] == 'y') ? 5 :   // May
           (__DATE__[0] == 'J' && __DATE__[2] == 'n') ? 6 :   // Jun
           (__DATE__[0] == 'J' && __DATE__[2] == 'l') ? 7 :   // Jul
           (__DATE__[0] == 'A' && __DATE__[1] == 'u') ? 8 :   // Aug
           (__DATE__[0] == 'S') ? 9 :                         // Sep
           (__DATE__[0] == 'O') ? 10 :                        // Oct
           (__DATE__[0] == 'N') ? 11 : 12;                    // Nov/Dec
}

// Парсинг дня (например, "Feb  4" или "Feb 14")
constexpr int BUILD_DAY() {
    return (__DATE__[4] == ' ' ? (__DATE__[5] - '0') : ((__DATE__[4] - '0') * 10 + (__DATE__[5] - '0')));
}

// Парсинг года (последние 2 цифры)
constexpr int BUILD_YEAR() {
    return (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0');
}

// Парсинг часов
constexpr int BUILD_HOUR() {
    return (__TIME__[0] - '0') * 10 + (__TIME__[1] - '0');
}

// Парсинг минут
constexpr int BUILD_MINUTE() {
    return (__TIME__[3] - '0') * 10 + (__TIME__[4] - '0');
}

// Глобальная статическая строка версии (Корректно: ГММДДЧЧММ, пример: 602111234)
// Г=последняя цифра года (2026->6), ММ,ДД,ЧЧ,ММ (минуты)
inline const char* getBuildVersion() {
    static char version[10] = {0};  // 9 + '\0'
    if (version[0] == 0) {
        int year = BUILD_YEAR();
        int month = BUILD_MONTH();
        int day = BUILD_DAY();
        int hour = BUILD_HOUR();
        int minute = BUILD_MINUTE();
        // Г=последняя цифра года
        snprintf(version, sizeof(version), "%1d%02d%02d%02d%02d",
                 year % 10, month, day, hour, minute); // Г ММ ДД ЧЧ ММ
    }
    return version;
}
// Числовая версия для сравнений (например: 2602041435UL)
constexpr unsigned long BUILD_VERSION_NUM() {
    return ((unsigned long)BUILD_YEAR() * 100000000UL) +
           ((unsigned long)BUILD_MONTH() * 1000000UL) +
           ((unsigned long)BUILD_DAY() * 10000UL) +
           ((unsigned long)BUILD_HOUR() * 100UL) +
           (unsigned long)BUILD_MINUTE();
}
// ===================== DEBUG FLAGS =====================
#define DEBUG_HX711   1 // HX711 verbose debug (raw vals, averaging)
#define DEBUG_SIM800 1 // 1 добавится только печать 
#define DEFAULT_GSM_ENABLED   true // true  false
#define DEBUG_GLOBAL 0  // Если включео - НЕ РАБОТАЕТ ОТА!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! Вся прочая отладка Serial — 1: включить, 0: убрать

// ======================= OTA ===========================
#ifndef OTA_CHECK_INTERVAL
#define OTA_CHECK_INTERVAL 1 // OTA check every X cycles
#endif
#define OTA_VERSION    getBuildVersion()  // Автоматическая версия ГГММДДЧЧММ

#define OTAUSER        "admin"
#define OTAPASSWORD    "662990"
#define OTAPATH        "/firmware"
#define SERVERPORT     80
#define OTA_PATH       "https://raw.githubusercontent.com/FLINN-AWI/solar/main/solar.json"

extern AutoOTA ota;

// ===================== RTC MAGIC =======================
#define RTC_MAGIC 0xA55A5AA6u

// =================== DEVICE DEFAULTS ===================
#define SLEEP_SEC     1
#define DEFAULT_DEVICE_NAME   "ESP8266_SOLAR"


// =================== VOLTAGE / ADC =====================
#define DEFAULT_VOLT_K        2.0f   // Voltage divider
#define DEFAULT_SCALE_K       22800.0f // HX711 scale

struct VoltageDivider {
    float K;
};

// ========== НАСТРОЙКИ DOMOFON ==========
#define DOMOFON_ENABLED  0 // 1 - включить DOMOFON, 0  - выключить DOMOFON


// ========== НАСТРОЙКИ VIESSMANN ==========
#define VIESSMANN_ENABLED  0 // 1 - включить управление котлом, 0  - выключить управление котлом

#define VIESSMANN_REG 1.50f      // регулятор температуры порог: 1.50 В (может быть 1.50, 1.55, 1.60, 1.65...)
//#define VIESSMANN_TIMER 3600         // ПЕРИОД сна в режиме VIESSMANN
// В этом интервале времени выполняется импульс включения (ON pulse)
#define VIESSMANN_ON_START_HOUR   6    // Час начала ON периода (6 утра)
#define VIESSMANN_ON_START_MIN    45   // Минуты начала ON периода (45 минут)
#define VIESSMANN_ON_END_HOUR     16   // Час окончания ON периода (10 вечера)
#define VIESSMANN_ON_END_MIN      30   // Минуты окончания ON периода (30 минут)

// ВРЕМЯ OFF ПЕРИОДА (например 22:30 - 6:45)
// В этом интервале времени выполняется импульс выключения (OFF pulse)
// Период OFF может переходить через полночь (как в примере с 22:30 до 6:45)
#define VIESSMANN_OFF_START_HOUR  17   // Час начала OFF периода (10 вечера)
#define VIESSMANN_OFF_START_MIN   30   // Минуты начала OFF периода (30 минут)
#define VIESSMANN_OFF_END_HOUR    6    // Час окончания OFF периода (6 утра следующего дня)
#define VIESSMANN_OFF_END_MIN     45   // Минуты окончания OFF периода (45 минут)

// Длительность импульсов (мс)
#define VIESSMANN_ON_PULSE_MS  3500   // для ON периода (на оба пина)
#define VIESSMANN_OFF_PULSE_MS 10000  // для OFF периода (только на SOLAR)

// ========================= PINOUT ======================
#define TX               1  //
#define RX               3  //                                                                 /OUT_HIGH
#define ONE_WIRE_BUS     0   // D3                                                               IN_HIGH/
#define PIN_CHARGE       2   // D4  (PIN_MIC_SD)                                                 IN_HIGH/OUT_LOW
#define PIN_SDA          4   // D2                                                                       /OUT_HIGH
#define PIN_SCL          5   // D1                                                                       /OUT_HIGH
#define SOL_GSM_RX     12  // D6 RX(TX GSM) or HX711_SCK                                      IN_HIGH/OUT_LOW
#define ACC_GSM_TX     13  // D7 TX(RX GSM) or HX711_DT     HX711 out-gpio13, sck-gpio12                                  IN_HIGH/OUT_LOW
#define PIR_AP_GND_SENS  14  // D5 PIR/AP input (PIN_MIC_WS), OUTPUT-ПИТАНИЕ BME280 + DS18B20    IN_HIGH/OUT_HIGH
#define GSM_POW_Ring_Zamok 15  // D8 ПРИ СТАРТЕ ПОДКЛЮЧЕН К ЗЕМЛЕ Charge sense (PIN_MIC_SCK)       IN_LOW/OUT_HIGH
#define WAKE_SLEEP       16  // D0                                                               IN_HIGH/OUT_LOW
// ========================= GSM ======================
//#define GSM_BAUD    9600 
#define SMS_NUMBER   "+79105817387"
#ifndef CALL_NUMBER1
#define CALL_NUMBER1 "+79105817387"
#endif
#ifndef CALL_NUMBER2  
#define CALL_NUMBER2 "+79166353990"
#endif
#ifndef CALL_NUMBER3
#define CALL_NUMBER3 "+71234567890"
#endif


#define MIN_SIM800_VOLTAGE    3.4f   // Минимальное напряжение для работы SIM800
#define LOW_BATTERY_ENTER     3.2f   // Порог входа в режим низкого заряда - В ТЕЛЕГРАММ ОТЧЕТ НЕ ОТПРАВЛЯЕТСЯ
#define LOW_BATTERY_EXIT      3.6f   // Порог выхода из режима низкого заряда

// ======================== WIFI + AP =====================
#define DEFAULT_PASS      "4601332222"
#define DEFAULT_WIFI_AP_1 "Xiaomi_0222"
#define DEFAULT_WIFI_AP_2 "home_flinn2"
#define DEFAULT_WIFI_AP_3 "Home_flinn2"
#define DEFAULT_WIFI_AP_4 "flinn_off"

// Dynamic settings (persisted to config.json)
struct ConfigData {
    String pass;
    String wifi1;
    String wifi2;
    String wifi3;
    String wifi4;
    String sms;
    String call;
    uint32_t sleepSec;
    String deviceName;
    bool gsmEnabled;
    float voltK;
    float scaleK;
};

extern ConfigData cfg;

// ======================== NTP ===========================
#define NTP_HOST    "pool.ntp.org"
#define NTP_OFFSET  10800 // Moscow (UTC+3)

// ================== TELEGRAM BOT ========================
//#define BOT_TOKEN   "8486734166:AAEULsHq5PmXiZpgEZbTXwZd8OsGQpOZz_s" //VIESSMANN
#define BOT_TOKEN   "8267607451:AAGanJQKpkZ971ht0QgmDvqZGBXB8kuyFZQ" // DOMOFON
//#define BOT_TOKEN   "7899080728:AAGpoPlr4ECV5BBCHQNmYNfCUXy0Khx_M94" // solar

//#define BOT_TOKEN   "8548492399:AAGZ_um0Ir7wA4h4r4v1ZXlVwGYGUdNJs30" //SCALE
#define CHAT_ID     "5297783183"
#define BOT2_TOKEN  "8514987120:AAHlz0sKjeee4P3sn3AsueRa-1ia8DAmRXw"
#define BOT2_CHATID "5297783183"
#define TG_TEXT_LIMIT 3800