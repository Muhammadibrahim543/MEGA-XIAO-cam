#pragma once
// ══════════════════════════════════════════════════════════════════
//  power_manager.h — Advanced ESP-IDF Power & Thermal Management
//  Seeed XIAO ESP32-S3 Sense
//  Features:
//    - Dynamic Frequency Scaling (240MHz, 160MHz, 80MHz)
//    - On-Chip Silicon Temperature Telemetry
//    - Automatic Thermal Throttling (> 75°C protection)
//    - Smart Auto-Dim / Light Sleep on Inactivity
//    - Deep Sleep with RTC Button Wakeup
// ══════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <TFT_eSPI.h>
#include "esp_pm.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

enum PowerProfile {
    PWR_PROFILE_AUTO = 0,    // Intelligently scales based on workload & thermals
    PWR_PROFILE_PERF,        // 240 MHz (Max FPS / Encoding)
    PWR_PROFILE_BALANCED,    // 160 MHz (Smooth UI, cool & efficient)
    PWR_PROFILE_ECO          // 80 MHz  (Ultra-low power & lowest heat)
};

struct PowerStats {
    float coreTempC;
    uint32_t currentFreqMhz;
    uint32_t targetFreqMhz;
    PowerProfile profile;
    const char* profileName;
    bool isThrottled;
    bool isDisplaySleeping;
    uint32_t lastActivityMs;
    uint32_t autoSleepTimeoutMs; // 0 = disabled
};

// ── Lifecycle ─────────────────────────────────────────────────────
void power_manager_init(TFT_eSPI* tftRef);
void power_manager_tick(bool isRecording, bool isStreaming, bool isHighLoad);

// ── Control ───────────────────────────────────────────────────────
void power_manager_set_profile(PowerProfile profile);
PowerProfile power_manager_get_profile();
const char* power_manager_get_profile_name(PowerProfile profile);

// ── Activity & Sleep ──────────────────────────────────────────────
void power_manager_activity();
void power_manager_set_timeout(uint32_t timeoutMs);
void power_manager_enter_light_sleep();
void power_manager_enter_deep_sleep();
void power_manager_wake_display();
void power_manager_sleep_display();

// ── Telemetry ─────────────────────────────────────────────────────
const PowerStats& power_manager_get_stats();
float power_manager_get_temperature();
uint32_t power_manager_get_cpu_freq();
bool power_manager_is_throttled();
