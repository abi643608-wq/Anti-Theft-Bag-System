#pragma once
#include <Arduino.h>

void beginTelegramAlerts();
void updateTelegramFix(bool valid, uint32_t capturedAt, double latitude, double longitude);
void setTelegramAlarmActive(uint32_t alarmId);
void queueTheftAlert(uint32_t alarmId);
void processTelegramResults();
