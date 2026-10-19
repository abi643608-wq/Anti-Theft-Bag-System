#include "TelegramAlerts.h"
#include "Secrets.h" // Generated only inside the ignored private build directory.
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

extern const uint8_t bundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t bundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace {
constexpr uint8_t maxAttempts = 5;
constexpr uint32_t maxBackoffMs = 30000;
constexpr uint32_t maxRateLimitWaitMs = 60000;
struct Fix { bool valid; uint32_t capturedAt; double latitude; double longitude; };
struct Job { uint32_t alarmId; bool followUp; };
struct Result { uint32_t alarmId; bool followUp; bool success; bool includedLocation; };
struct Reply { bool success; bool permanent; uint32_t retryAfterMs; bool includedLocation; };
QueueHandle_t jobs = nullptr;
QueueHandle_t results = nullptr;
portMUX_TYPE sharedLock = portMUX_INITIALIZER_UNLOCKED;
Fix latestFix = {};
uint32_t activeAlarmId = 0;
// These delivery flags belong to the Arduino loop only.
bool primaryDelivered = false;
bool primaryHadLocation = false;
bool followUpQueued = false;

Fix copyFix() {
  portENTER_CRITICAL(&sharedLock);
  const Fix fix = latestFix;
  portEXIT_CRITICAL(&sharedLock);
  return fix;
}
uint32_t copyActiveAlarm() {
  portENTER_CRITICAL(&sharedLock);
  const uint32_t id = activeAlarmId;
  portEXIT_CRITICAL(&sharedLock);
  return id;
}
bool fresh(const Fix &fix) {
  return fix.valid && millis() - fix.capturedAt < 5000;
}
bool networkReady() {
  return WiFi.status() == WL_CONNECTED && time(nullptr) >= 1700000000;
}

Reply requestTelegram(const Job *job) {
  Reply reply = {false, false, 0, false};
  if (!networkReady()) return reply; // Never bypass certificate-time verification.
  if (job && job->followUp && copyActiveAlarm() != job->alarmId) {
    reply.permanent = true;
    return reply;
  }
  const Fix fix = copyFix();
  if (job && job->followUp && !fresh(fix)) return reply;

  NetworkClientSecure client;
  client.setCACertBundle(bundleStart, (size_t)(bundleEnd - bundleStart));
  client.setHandshakeTimeout(8);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const String endpoint = String("https://api.telegram.org/bot") + BOT_TOKEN +
                          (job ? "/sendMessage" : "/getMe");
  if (!http.begin(client, endpoint)) return reply;

  int code;
  if (!job) {
    code = http.GET(); // Read-only startup check; no test message is sent.
  } else {
    // Recheck freshness after any connection preparation, on every attempt.
    const Fix current = copyFix();
    const bool includeLocation = fresh(current);
    if (job->followUp && (!includeLocation || copyActiveAlarm() != job->alarmId)) {
      http.end();
      return reply;
    }
    String message = job->followUp ? "Backpack fresh-location update" : "THEFT ALERT: Backpack opened while armed";
    message += "\nAlarm #" + String(job->alarmId);
    if (includeLocation) {
      message += "\nhttps://www.google.com/maps?q=" + String(current.latitude, 6) + "," + String(current.longitude, 6);
    } else {
      message += "\nLocation unavailable";
    }
    JsonDocument body;
    body["chat_id"] = TELEGRAM_CHAT_ID;
    body["text"] = message;
    body["link_preview_options"]["is_disabled"] = true;
    String payload;
    serializeJson(body, payload);
    http.addHeader("Content-Type", "application/json");
    code = http.POST(payload);
    reply.includedLocation = includeLocation;
  }
  if (code > 0) {
    JsonDocument filter;
    filter["ok"] = true;
    filter["error_code"] = true;
    filter["parameters"]["retry_after"] = true;
    JsonDocument response;
    const auto error = deserializeJson(response, http.getString(), DeserializationOption::Filter(filter));
    reply.success = code == 200 && !error && response["ok"].as<bool>();
    const int apiCode = error ? code : (response["error_code"] | code);
    reply.permanent = !reply.success && apiCode >= 400 && apiCode < 500 && apiCode != 429;
    const uint32_t seconds = response["parameters"]["retry_after"] | 0U;
    reply.retryAfterMs = seconds > 60 ? maxRateLimitWaitMs : seconds * 1000;
  }
  http.end();
  // Never print endpoint, payload, response body, SSID, password or token.
  return reply;
}

void networkTask(void *) {
  esp_log_level_set("*", ESP_LOG_NONE); // Suppress SDK diagnostics that could expose configuration.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  bool connectionReported = false;
  bool readinessVerified = false;
  uint32_t lastReconnect = millis();
  uint32_t lastReadinessCheck = millis() - 30000;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!connectionReported) {
        Serial.println("Wi-Fi connected; waiting for verified Telegram HTTPS readiness.");
        connectionReported = true;
      }
    } else {
      connectionReported = false;
      if (millis() - lastReconnect >= 15000) {
        lastReconnect = millis();
        WiFi.reconnect();
      }
    }
    Job job;
    if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(250)) == pdTRUE) {
      Reply reply = {};
      for (uint8_t attempt = 0; attempt < maxAttempts; ++attempt) {
        if (job.followUp && copyActiveAlarm() != job.alarmId) break;
        reply = requestTelegram(&job);
        if (reply.success || reply.permanent) break;
        Serial.printf("Telegram %s attempt %u failed (alarm %lu).\n",
                      job.followUp ? "location update" : "theft alert", attempt + 1, (unsigned long)job.alarmId);
        if (attempt + 1 < maxAttempts) {
          uint32_t waitMs = 2000UL << attempt;
          if (waitMs > maxBackoffMs) waitMs = maxBackoffMs;
          if (reply.retryAfterMs > waitMs) waitMs = reply.retryAfterMs;
          vTaskDelay(pdMS_TO_TICKS(waitMs)); // Sleeps this task only.
        }
      }
      const Result result = {job.alarmId, job.followUp, reply.success, reply.includedLocation};
      xQueueSend(results, &result, 0);
      Serial.printf("Telegram %s %s (alarm %lu).\n", job.followUp ? "location update" : "theft alert",
                    reply.success ? "sent" : "not delivered", (unsigned long)job.alarmId);
    } else if (!readinessVerified && networkReady() && millis() - lastReadinessCheck >= 30000) {
      lastReadinessCheck = millis();
      readinessVerified = requestTelegram(nullptr).success;
      if (readinessVerified) Serial.println("Telegram certificate-verified HTTPS and bot authentication ready.");
      else Serial.println("Telegram HTTPS readiness check failed; will retry later.");
    }
  }
}
} // namespace

void beginTelegramAlerts() {
  jobs = xQueueCreate(16, sizeof(Job));
  results = xQueueCreate(16, sizeof(Result));
  if (!jobs || !results || xTaskCreatePinnedToCore(networkTask, "Telegram", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
    if (jobs) vQueueDelete(jobs);
    if (results) vQueueDelete(results);
    jobs = results = nullptr;
    Serial.println("Telegram task unavailable; local alarm remains active.");
  }
}
void updateTelegramFix(bool valid, uint32_t capturedAt, double latitude, double longitude) {
  portENTER_CRITICAL(&sharedLock);
  latestFix = {valid, capturedAt, latitude, longitude};
  portEXIT_CRITICAL(&sharedLock);
}
void setTelegramAlarmActive(uint32_t alarmId) {
  portENTER_CRITICAL(&sharedLock);
  activeAlarmId = alarmId;
  portEXIT_CRITICAL(&sharedLock);
  primaryDelivered = primaryHadLocation = followUpQueued = false;
}
void queueTheftAlert(uint32_t alarmId) {
  const Job job = {alarmId, false};
  if (!jobs || xQueueSend(jobs, &job, 0) != pdTRUE) {
    Serial.println("Telegram alert queue unavailable or full; local alarm remains active.");
  }
}
void processTelegramResults() {
  if (!results) return;
  Result result;
  const uint32_t id = copyActiveAlarm();
  while (xQueueReceive(results, &result, 0) == pdTRUE) {
    if (id && result.alarmId == id && !result.followUp) {
      primaryDelivered = result.success;
      primaryHadLocation = result.includedLocation;
    }
  }
  if (id && primaryDelivered && !primaryHadLocation && !followUpQueued && fresh(copyFix())) {
    const Job job = {id, true};
    if (xQueueSend(jobs, &job, 0) == pdTRUE) followUpQueued = true;
  }
}
