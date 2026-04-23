// BUILD_TIMESTAMP: 2026-04-04 21:54:41
// rtc.cpp - Хранение данных в RTC memory ESP8266
#include "rtc.h"
#include <user_interface.h>

// Глобальный объект RTC данных
RTCStore rtc;

// ===============================================================
//                    РАБОТА С RTC MEMORY ESP8266
// ===============================================================

bool rtcMemLoad() {
    system_rtc_mem_read(RTC_MEMORY_ADDR, (uint8_t*)&rtc, sizeof(RTCStore));
    
    Serial.printf("[RTC/MEM] LOAD: magic=0x%08X, size=%d bytes\n", 
                  rtc.magic, sizeof(RTCStore));
    
    if (rtc.magic != RTC_MAGIC) {
        Serial.printf("[RTC/MEM] INVALID: magic mismatch (expected 0x%08X)\n", RTC_MAGIC);
        return false;
    }
    
    uint32_t calculated = calcChecksum(&rtc);
    if (calculated != rtc.checksum) {
        Serial.printf("[RTC/MEM] INVALID: checksum mismatch (calc=0x%08X, stored=0x%08X)\n", 
                      calculated, rtc.checksum);
        return false;
    }
    
    Serial.printf("[RTC/MEM] VALID: wakeHour=%u, dailySendCount=%u, baseTimestamp=%u\n",
                  rtc.wakeHour, rtc.dailySendCount, rtc.baseTimestamp);
    Serial.printf("[RTC/MEM] reserved3=%u, flags=0x%02X\n",
                  rtc.reserved3, rtc.flags);
    Serial.printf("[RTC/MEM] lowBatteryMode=%s, onExecuted=%s, offExecuted=%s, sim800Present=%s\n",
                  isLowBatteryMode() ? "yes" : "no",
                  isOnExecuted() ? "yes" : "no",
                  isOffExecuted() ? "yes" : "no",
                  isSim800Present() ? "yes" : "no");
    
    return true;
}

void rtcMemSave() {
    rtc.checksum = calcChecksum(&rtc);
    system_rtc_mem_write(RTC_MEMORY_ADDR, (uint8_t*)&rtc, sizeof(RTCStore));
    
    Serial.printf("[RTC/MEM] SAVED: magic=0x%08X, size=%d bytes\n", 
                  rtc.magic, sizeof(RTCStore));
    Serial.printf("[RTC/MEM] wakeHour=%u, dailySendCount=%u, baseTimestamp=%u\n",
                  rtc.wakeHour, rtc.dailySendCount, rtc.baseTimestamp);
}

void rtcMemInit() {
    Serial.printf("[RTC/MEM] Initializing with defaults\n");
    
    memset(&rtc, 0, sizeof(RTCStore));
    
    rtc.magic = RTC_MAGIC;
    rtc.wakeHour = 0;
    rtc.baseTimestamp = 0;
    rtc.dailySendCount = 0;
    rtc.lowBatteryHours = 0;
    rtc.flags = 0;  // Все флаги сброшены
    rtc.rehabHours = 0;
    rtc.reserved3 = 0;
    
    memset(rtc.wifiBssid, 0, 6);
    rtc.wifiChannel = 0;
    
    rtcMemSave();
    
    Serial.printf("[RTC/MEM] Initialization complete\n");
}

uint32_t calcChecksum(RTCStore* r) {
    const uint8_t* p = (uint8_t*)r;
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof(RTCStore) - sizeof(r->checksum); i++) {
        sum += p[i];
    }
    return sum;
}

// ===============================================================
//                    ФУНКЦИИ ДЛЯ СОВМЕСТИМОСТИ
// ===============================================================

void rtcLoad() {
    if (!rtcMemLoad()) {
        Serial.printf("[RTC] No valid data found, initializing\n");
        rtcMemInit();
    }
}

void rtcSave() {
    rtcMemSave();
}

size_t rtcGetSize() {
    return sizeof(RTCStore);
}
// ... (в конец файла rtc.cpp)

void rtcDebugDumpToSerial(const char* tag) {
#if DEBUG_GLOBAL
    Serial.printf("\n[RTC/DUMP]%s%s\n", tag ? " " : "", tag ? tag : "");
    Serial.printf("[RTC/DUMP] sizeof(RTCStore)=%u magic=0x%08X (expected 0x%08X) checksum=0x%08X\n",
                  (unsigned)sizeof(RTCStore), rtc.magic, (unsigned)RTC_MAGIC, rtc.checksum);

    Serial.printf("[RTC/DUMP] wakeHour=%u baseTimestamp=%u dailySendCount=%u\n",
                  rtc.wakeHour, rtc.baseTimestamp, rtc.dailySendCount);

    Serial.printf("[RTC/DUMP] lowBatteryHours=%u rehabHours=%u flags=0x%02X\n",
                  rtc.lowBatteryHours, rtc.rehabHours, rtc.flags);

    Serial.printf("[RTC/DUMP] flags: lowBatteryMode=%s onExecuted=%s offExecuted=%s sim800Present=%s\n",
                  isLowBatteryMode() ? "YES" : "NO",
                  isOnExecuted() ? "YES" : "NO",
                  isOffExecuted() ? "YES" : "NO",
                  isSim800Present() ? "YES" : "NO");

    Serial.printf("[RTC/DUMP] last WiFi: ch=%u bssid=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  rtc.wifiChannel,
                  rtc.wifiBssid[0], rtc.wifiBssid[1], rtc.wifiBssid[2],
                  rtc.wifiBssid[3], rtc.wifiBssid[4], rtc.wifiBssid[5]);

    // Печать всех 24 часов в логическом порядке 0..23
    for (int h = 0; h < 24; h++) {
        const HourRecord& r = rtcHourConst((uint8_t)h);

        bool empty = (r.t_ds5 == 0 && r.t_bme5 == 0 && r.hum5 == 0 &&
                      r.pressDelta == 0 && r.acc50 == 0 && r.solar50 == 0 &&
                      r.charge50 == 0 && r.weight2 == 0);

        float t_ds   = (r.t_ds5 == -127) ? -127.0f : (r.t_ds5 / 2.0f);
        float t_bme  = (r.t_bme5 == -127) ? -127.0f : (r.t_bme5 / 2.0f);
float hum    = (float)r.hum5;
        float press  = 900.0f + r.pressDelta;
        float acc    = r.acc50 / 50.0f;
        float solar  = r.solar50 / 50.0f;
        float charge = r.charge50 / 50.0f;
        float weight = r.weight2 / 2.0f;

        if (empty) {
            Serial.printf("[RTC/DUMP] H%02d: <empty>\n", h);
        } else {
            Serial.printf("[RTC/DUMP] H%02d: tDS=%5.1f tBME=%5.1f hum=%4.0f press=%4.0f acc=%4.2f sol=%4.2f chg=%4.2f w=%5.1f\n",
                          h, t_ds, t_bme, hum, press, acc, solar, charge, weight);
        }
    }
    Serial.printf("[RTC/DUMP] END\n\n");
#endif
}