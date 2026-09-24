// ══════════════════════════════════════════════════════════════════
//  power_manager.cpp — Advanced ESP-IDF Power & Thermal Management
//  Seeed XIAO ESP32-S3 Sense
// ══════════════════════════════════════════════════════════════════
#include "power_manager.h"

#define BTN_PIN_OK   2
#define BTN_PIN_UP   1
#define BTN_PIN_DN   43

#define THERMAL_CRITICAL_TEMP   82.0f   // Emergency throttle to 80MHz
#define THERMAL_THROTTLE_TEMP   75.0f   // Throttle to 160MHz
#define THERMAL_RECOVERY_TEMP   68.0f   // Return to desired profile

static TFT_eSPI* pTft = nullptr;
static PowerStats stats = {
    .coreTempC = 0.0f,
    .currentFreqMhz = 240,
    .targetFreqMhz = 240,
    .profile = PWR_PROFILE_AUTO,
    .profileName = "AUTO",
    .isThrottled = false,
    .isDisplaySleeping = false,
    .lastActivityMs = 0,
    .autoSleepTimeoutMs = 0 // 0 = disabled by default
};

static uint32_t lastThermalCheckMs = 0;
static uint32_t currentSetFreq = 240;

static void applyCpuFrequency(uint32_t freqMhz) {
    setCpuFrequencyMhz(freqMhz);
    currentSetFreq = getCpuFrequencyMhz();
    stats.currentFreqMhz = currentSetFreq;
    Serial.printf("[PWR] CPU Frequency adjusted to %lu MHz (target %lu MHz)\n", 
                  (unsigned long)currentSetFreq, (unsigned long)freqMhz);
}


void power_manager_init(TFT_eSPI* tftRef) {
    pTft = tftRef;
    stats.coreTempC = temperatureRead();
    currentSetFreq = getCpuFrequencyMhz();
    stats.currentFreqMhz = currentSetFreq;
    stats.targetFreqMhz = currentSetFreq;
    stats.lastActivityMs = millis();
    stats.profile = PWR_PROFILE_AUTO;
    stats.profileName = "AUTO";
    Serial.printf("[PWR] Initialized: CPU=%lu MHz, CoreTemp=%.1f C\n", 
                  (unsigned long)stats.currentFreqMhz, stats.coreTempC);
}

void power_manager_activity() {
    stats.lastActivityMs = millis();
    if (stats.isDisplaySleeping) {
        power_manager_wake_display();
    }
}

void power_manager_set_timeout(uint32_t timeoutMs) {
    stats.autoSleepTimeoutMs = timeoutMs;
}

void power_manager_set_profile(PowerProfile profile) {
    stats.profile = profile;
    stats.profileName = power_manager_get_profile_name(profile);
    stats.isThrottled = false;

    switch (profile) {
        case PWR_PROFILE_PERF:
            stats.targetFreqMhz = 240;
            break;
        case PWR_PROFILE_BALANCED:
            stats.targetFreqMhz = 160;
            break;
        case PWR_PROFILE_ECO:
            stats.targetFreqMhz = 80;
            break;
        case PWR_PROFILE_AUTO:
        default:
            stats.targetFreqMhz = 240;
            break;
    }
    applyCpuFrequency(stats.targetFreqMhz);
}

PowerProfile power_manager_get_profile() {
    return stats.profile;
}

const char* power_manager_get_profile_name(PowerProfile profile) {
    switch (profile) {
        case PWR_PROFILE_PERF:     return "PERF";
        case PWR_PROFILE_BALANCED: return "BALANCED";
        case PWR_PROFILE_ECO:      return "ECO";
        case PWR_PROFILE_AUTO:     return "AUTO";
        default:                   return "UNKNOWN";
    }
}

void power_manager_sleep_display() {
    if (!stats.isDisplaySleeping && pTft != nullptr) {
        // ST7789 Sleep In command over SPI
        pTft->writecommand(0x10);
        stats.isDisplaySleeping = true;
        Serial.println("[PWR] Display entered low-power sleep mode");
    }
}

void power_manager_wake_display() {
    if (stats.isDisplaySleeping && pTft != nullptr) {
        // ST7789 Sleep Out command over SPI
        pTft->writecommand(0x11);
        delay(10);
        stats.isDisplaySleeping = false;
        Serial.println("[PWR] Display woke up from sleep mode");
    }
}

void power_manager_enter_light_sleep() {
    Serial.println("[PWR] Entering ESP-IDF Light Sleep (wake with buttons)...");
    Serial.flush();

    power_manager_sleep_display();

    // Configure GPIO wakeups for buttons (active LOW)
    gpio_wakeup_enable((gpio_num_t)BTN_PIN_OK, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)BTN_PIN_UP, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    // Enter light sleep
    esp_light_sleep_start();

    // Woken up
    power_manager_activity();
    power_manager_wake_display();
    Serial.println("[PWR] Woke up from Light Sleep!");
}

void power_manager_enter_deep_sleep() {
    Serial.println("[PWR] Entering Deep Sleep (wake with OK button)...");
    Serial.flush();

    power_manager_sleep_display();

    // Configure RTC GPIO wakeup on OK button (GPIO 2, active LOW)
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_PIN_OK, 0);

    delay(100);
    esp_deep_sleep_start();
}

void power_manager_tick(bool isRecording, bool isStreaming, bool isHighLoad) {
    uint32_t now = millis();

    // 1. Periodic Temperature & Thermal Management (every 1 second)
    if (now - lastThermalCheckMs >= 1000) {
        lastThermalCheckMs = now;
        stats.coreTempC = temperatureRead();

        // Thermal throttling logic
        if (stats.coreTempC >= THERMAL_CRITICAL_TEMP) {
            if (!stats.isThrottled || stats.currentFreqMhz > 80) {
                stats.isThrottled = true;
                applyCpuFrequency(80);
                Serial.printf("[PWR] CRITICAL HEAT WARNING: %.1f C! Throttling to 80 MHz!\n", stats.coreTempC);
            }
        } else if (stats.coreTempC >= THERMAL_THROTTLE_TEMP) {
            if (!stats.isThrottled || stats.currentFreqMhz > 160) {
                stats.isThrottled = true;
                applyCpuFrequency(160);
                Serial.printf("[PWR] THERMAL THROTTLE: %.1f C. Throttling to 160 MHz.\n", stats.coreTempC);
            }
        } else if (stats.isThrottled && stats.coreTempC <= THERMAL_RECOVERY_TEMP) {
            stats.isThrottled = false;
            Serial.printf("[PWR] Thermal normalized (%.1f C). Restoring target profile.\n", stats.coreTempC);
            applyCpuFrequency(stats.targetFreqMhz);
        }

        // Auto Frequency scaling if in AUTO mode and not thermally throttled
        if (stats.profile == PWR_PROFILE_AUTO && !stats.isThrottled) {
            uint32_t autoTarget = 160; // Balanced default

            if (isRecording || isStreaming || isHighLoad) {
                autoTarget = 240; // Max speed for demanding video/encoding tasks
            } else {
                autoTarget = 160; // Viewfinder & UI navigation is buttery smooth at 160MHz
            }

            if (autoTarget != stats.targetFreqMhz) {
                stats.targetFreqMhz = autoTarget;
                applyCpuFrequency(autoTarget);
            }
        }
    }

    // 2. Auto Display Sleep on Inactivity
    if (stats.autoSleepTimeoutMs > 0 && !stats.isDisplaySleeping && !isRecording && !isStreaming) {
        if (now - stats.lastActivityMs >= stats.autoSleepTimeoutMs) {
            power_manager_sleep_display();
            // Drop to 80MHz while display is sleeping to save power
            if (!stats.isThrottled) {
                applyCpuFrequency(80);
            }
        }
    }
}

const PowerStats& power_manager_get_stats() {
    return stats;
}

float power_manager_get_temperature() {
    return stats.coreTempC;
}

uint32_t power_manager_get_cpu_freq() {
    return stats.currentFreqMhz;
}

bool power_manager_is_throttled() {
    return stats.isThrottled;
}
