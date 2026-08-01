#pragma once

#include <stdint.h>
#include <TFT_eSPI.h>

void wifiStreamInit();
void wifiStreamEnter();
void wifiStreamLeave();
void wifiStreamSelectAction();
void wifiStreamTick();
void drawWiFiStream(TFT_eSprite& spFeed, TFT_eSprite& spMenu);
