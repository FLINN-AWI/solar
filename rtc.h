// BUILD_TIMESTAMP: 2026-04-04 21:54:41
// rtc.h
#pragma once

#include <Arduino.h>
#include "config.h"

// ====================== Hour record (8 bytes) ======================
struct __attribute__((packed)) HourRecord {
    int8_t  t_ds5;      // DS18B20: 0.5°C steps, -127 = invalid/no sensor
    int8_t  t_bme5;     // BME280:  0.5°C steps, -127 = invalid/no sensor
uint8_t hum5;       // humidity % (0..100), - "raw" percent to fit uint8_t
    uint8_t pressDelta; // pressure - 900 hPa (0..255)
    uint8_t acc50;      // battery V * 50  (0.02V steps)
    uint8_t solar50;    // solar V * 50
    uint8_t charge50;   // charge V * 50
    uint8_t weight2;    // weight kg * 2 (0.5kg steps)
};

// ====================== Flags bits ======================
static constexpr uint8_t FLAG_LOW_BATTERY_MODE = 0x01; // bit0
static constexpr uint8_t FLAG_ON_EXECUTED      = 0x02; // bit1
static constexpr uint8_t FLAG_OFF_EXECUTED     = 0x04; // bit2
static constexpr uint8_t FLAG_SIM800_PRESENT   = 0x08; // bit3
// bits 4..7 free

// ====================== RTC storage layout ======================
struct __attribute__((packed)) RTCStore {
    uint32_t magic;           // RTC_MAGIC (from config.h or fallback below)
    uint8_t  wakeHour;        // 0..24 (project logic)
    uint8_t  reserved1;
    uint8_t  reserved2;
    uint8_t  reserved3;       // first-cycle flag/service
    uint32_t baseTimestamp;   // UTC epoch

    uint8_t  wifiBssid[6];    // last connected BSSID
    uint8_t  wifiChannel;     // last connected channel

    uint16_t dailySendCount;

    HourRecord hours[24];

    uint8_t  lowBatteryHours;
    uint8_t  flags;           // FLAG_...
    uint8_t  rehabHours;
    uint32_t checksum;        // checksum of all bytes except this field
};

// ====================== Constants ======================
#ifndef RTC_MAGIC
#define RTC_MAGIC 0x52A71173UL
#endif

#ifndef RTC_MEMORY_ADDR
#define RTC_MEMORY_ADDR 64
#endif

// ====================== Global RTC object ======================
extern RTCStore rtc;

// ====================== Compile-time size checks ======================
static_assert(sizeof(HourRecord) == 8, "HourRecord must be exactly 8 bytes");
static_assert(sizeof(RTCStore)   == (4 + 4 + 4 + 7 + 2 + 24*8 + 3 + 4),
              "RTCStore size changed unexpectedly; this will invalidate RTC data");

// ====================== Hour indexing helpers ======================
// Mapping used in code: logical hour 0 -> hours[23], hour 23 -> hours[0]
static inline uint8_t hourToRtcIndex(uint8_t hour) {
    return (uint8_t)(23 - (hour % 24));
}

static inline HourRecord& rtcHour(uint8_t hour) {
    return rtc.hours[hourToRtcIndex(hour)];
}

static inline const HourRecord& rtcHourConst(uint8_t hour) {
    return rtc.hours[hourToRtcIndex(hour)];
}

// ====================== Flag helpers ======================
static inline bool isLowBatteryMode() { return (rtc.flags & FLAG_LOW_BATTERY_MODE) != 0; }
static inline void setLowBatteryMode(bool on) {
    if (on) rtc.flags |= FLAG_LOW_BATTERY_MODE; else rtc.flags &= (uint8_t)~FLAG_LOW_BATTERY_MODE;
}

static inline bool isOnExecuted() { return (rtc.flags & FLAG_ON_EXECUTED) != 0; }
static inline void setOnExecuted(bool on) {
    if (on) rtc.flags |= FLAG_ON_EXECUTED; else rtc.flags &= (uint8_t)~FLAG_ON_EXECUTED;
}

static inline bool isOffExecuted() { return (rtc.flags & FLAG_OFF_EXECUTED) != 0; }
static inline void setOffExecuted(bool on) {
    if (on) rtc.flags |= FLAG_OFF_EXECUTED; else rtc.flags &= (uint8_t)~FLAG_OFF_EXECUTED;
}

static inline bool isSim800Present() { return (rtc.flags & FLAG_SIM800_PRESENT) != 0; }
static inline void setSim800Present(bool on) {
    if (on) rtc.flags |= FLAG_SIM800_PRESENT; else rtc.flags &= (uint8_t)~FLAG_SIM800_PRESENT;
}

// ====================== RTC memory API ======================
uint32_t calcChecksum(RTCStore* r);
bool rtcMemLoad();
void rtcMemSave();
void rtcMemInit();

// compatibility/aliases
void rtcLoad();
void rtcSave();
size_t rtcGetSize();
void rtcDebugDumpToSerial(const char* tag = nullptr);