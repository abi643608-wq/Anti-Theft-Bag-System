#include <Keypad.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#include "TelegramAlerts.h"

// Verified keypad mapping: rows top-to-bottom, columns left-to-right.
byte rowPins[4] = {33, 32, 27, 26};
byte colPins[4] = {25, 23, 19, 18};
char keys[4][4] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
Keypad keypad(makeKeymap(keys), rowPins, colPins, 4, 4);

const byte reedPin = 35;
const byte buzzerPin = 14; // Active-high buzzer.
const unsigned long reedDebounceMs = 50;
int reedCandidate = HIGH;
int reedStable = -1;
unsigned long reedCandidateSince = 0;

enum AlarmState { DISARMED, ARMED, ALARM_LATCHED };
AlarmState alarmState = DISARMED;
char pinEntry[5] = {};
byte pinLength = 0;
bool pinOverflow = false;

const int8_t gpsRxPin = 16;
const int8_t gpsTxPin = -1;
const unsigned long maximumFixAgeMs = 5000;
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;
bool alarmLocationPrinted = false;
unsigned long lastGpsReportMs = 0;
uint32_t alarmSequence = 0;

bool hasFreshLocation() {
  return gps.location.isValid() && gps.location.age() < maximumFixAgeMs;
}

void readGps() {
  // Bound each batch so a busy UART cannot starve keypad or reed checks.
  for (byte count = 0; count < 64 && gpsSerial.available() > 0; ++count) {
    gps.encode((char)gpsSerial.read());
  }
}

void reportAlarmLocation(bool initialReport) {
  if (alarmState != ALARM_LATCHED || alarmLocationPrinted) return;
  if (hasFreshLocation()) {
    Serial.print("Google Maps: https://www.google.com/maps?q=");
    Serial.print(gps.location.lat(), 6);
    Serial.print(',');
    Serial.println(gps.location.lng(), 6);
    alarmLocationPrinted = true;
  } else if (initialReport) {
    Serial.println("Location unavailable");
  }
}

void printGpsStatus() {
  Serial.print("Satellites: ");
  if (gps.satellites.isValid() && gps.satellites.age() < maximumFixAgeMs) {
    Serial.print(gps.satellites.value());
  } else {
    Serial.print("unknown");
  }
  Serial.print(" | Fix: ");
  if (hasFreshLocation()) {
    Serial.print("VALID | Latitude: ");
    Serial.print(gps.location.lat(), 6);
    Serial.print(" | Longitude: ");
    Serial.println(gps.location.lng(), 6);
  } else {
    Serial.println("NO VALID RECENT FIX | Waiting for GPS fix");
  }
}

void clearPin() {
  pinLength = 0;
  pinEntry[0] = '\0';
  pinOverflow = false;
}

void printStatus() {
  Serial.print("STATUS: ");
  if (alarmState == DISARMED) Serial.println("DISARMED");
  else if (alarmState == ARMED) Serial.println("ARMED");
  else Serial.println("ALARM LATCHED");
}

void updateReed() {
  const unsigned long now = millis();
  const int reading = digitalRead(reedPin);
  if (reading != reedCandidate) {
    reedCandidate = reading;
    reedCandidateSince = now;
  }
  // Nonblocking debounce; unsigned subtraction tolerates millis() rollover.
  if (reedCandidate != reedStable && now - reedCandidateSince >= reedDebounceMs) {
    reedStable = reedCandidate;
    Serial.println(reedStable == LOW ? "BAG CLOSED" : "BAG OPEN");
    if (alarmState == ARMED && reedStable == HIGH) {
      alarmState = ALARM_LATCHED;
      alarmLocationPrinted = false;
      ++alarmSequence;
      setTelegramAlarmActive(alarmSequence);
      queueTheftAlert(alarmSequence);
      digitalWrite(buzzerPin, HIGH);
      Serial.println("ALARM TRIGGERED - BUZZER ON");
      printStatus();
      reportAlarmLocation(true);
    }
    // Closing the reed cannot clear a latched alarm.
  }
}

void handleKey(char key) {
  if (key == '*') {
    clearPin();
    Serial.println("PIN ENTRY CLEARED");
  } else if (key == '#') {
    const bool correctPin = !pinOverflow && pinLength == 4 &&
      pinEntry[0] == '1' && pinEntry[1] == '2' &&
      pinEntry[2] == '3' && pinEntry[3] == '4';
    clearPin();
    if (correctPin) {
      alarmState = DISARMED;
      setTelegramAlarmActive(0);
      alarmLocationPrinted = false;
      digitalWrite(buzzerPin, LOW);
      Serial.println("PIN ACCEPTED - BUZZER OFF");
    } else {
      Serial.println("INCORRECT PIN");
    }
    printStatus();
  } else if (key == 'A') {
    if (alarmState != DISARMED) {
      Serial.println("ALREADY ARMED - USE PIN 1234# TO DISARM");
    } else if (reedStable != LOW || reedCandidate != LOW ||
               millis() - reedCandidateSince < reedDebounceMs) {
      Serial.println("ARM REJECTED - MAGNET MUST BE NEARBY AND STABLE");
    } else {
      clearPin();
      alarmState = ARMED;
      alarmLocationPrinted = false;
      digitalWrite(buzzerPin, LOW);
      Serial.println("ARMED - BUZZER OFF");
      printStatus();
    }
  } else if (key >= '0' && key <= '9') {
    if (pinLength < 4 && !pinOverflow) {
      pinEntry[pinLength++] = key;
      pinEntry[pinLength] = '\0';
      Serial.print("PIN DIGITS: ");
      Serial.println(pinLength);
    } else {
      pinOverflow = true; // Never accept 12345# by silently truncating it.
      Serial.println("PIN TOO LONG - PRESS * TO CLEAR");
    }
  } else {
    Serial.print("KEY IGNORED: ");
    Serial.println(key);
  }
}

void setup() {
  digitalWrite(buzzerPin, LOW);
  pinMode(buzzerPin, OUTPUT);
  pinMode(reedPin, INPUT); // External 10k pull-up to 3V3, reed to GND.
  for (byte i = 0; i < 4; ++i) {
    pinMode(rowPins[i], INPUT_PULLUP);
    pinMode(colPins[i], INPUT_PULLUP);
  }
  keypad.setDebounceTime(25);
  Serial.begin(115200);
  gpsSerial.begin(9600, SERIAL_8N1, gpsRxPin, gpsTxPin);
  Serial.println("KEYPAD + REED + GPS ALARM - 115200 baud");
  Serial.println("A: arm with magnet nearby. 1234#: disarm. *: clear PIN.");
  Serial.println("Rows: 33,32,27,26; columns: 25,23,19,18.");
  Serial.println("GPS RX: GPIO16, 9600 baud, TX disabled; fix must be under 5 seconds old.");
  printStatus();
  Serial.println("BUZZER OFF");
  reedCandidate = digitalRead(reedPin);
  reedCandidateSince = millis();
  lastGpsReportMs = millis();
  beginTelegramAlerts();
}

void loop() {
  readGps();
  const bool freshFix = hasFreshLocation();
  updateTelegramFix(freshFix, freshFix ? millis() - gps.location.age() : 0,
                    freshFix ? gps.location.lat() : 0, freshFix ? gps.location.lng() : 0);
  updateReed();
  const char key = keypad.getKey();
  if (key) handleKey(key);
  reportAlarmLocation(false); // One deferred link when a fresh fix arrives.
  processTelegramResults();
  const unsigned long now = millis();
  if (now - lastGpsReportMs >= 1000) {
    lastGpsReportMs = now;
    printGpsStatus();
  }
}
