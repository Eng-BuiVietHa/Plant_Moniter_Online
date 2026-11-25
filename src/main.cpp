#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <Preferences.h>
#include "../include/secrets.h"
#include <time.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Cấu hình máy chủ giờ NTP (Múi giờ Việt Nam UTC+7)
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 7 * 3600; // GMT+7
const int DAYLIGHT_OFFSET_SEC = 0;

// ================================================================
// 2. CẤU HÌNH PHẦN CỨNG (ESP32 DEVKIT V1)
// ================================================================
const int PIN_DHT11 = 4;  // Chân D4 (GPIO 4) nhận dữ liệu từ cảm biến DHT11
const int PIN_RELAY = 18; // Chân D18 (GPIO 18) xuất tín hiệu điều khiển Relay

// Cấu hình Relay kích MỨC CAO (Active HIGH)
#define RELAY_ON HIGH // Xuất 3.3V - 5V -> Máy bơm BẬT
#define RELAY_OFF LOW // Kéo xuống 0V -> Máy bơm TẮT

// ================================================================
// 3. BIẾN TOÀN CỤC & ĐỐI TƯỢNG HỆ THỐNG
// ================================================================
DHTesp dht;
bool currentRelayState = false;
bool relayStateNeedsPublish = false;
portMUX_TYPE pumpStateMux = portMUX_INITIALIZER_UNLOCKED;

// Cấu hình đồng bộ từ Firebase
enum ControlMode : uint8_t { MODE_OFF, MODE_AUTO, MODE_MANUAL };
volatile ControlMode currentMode = MODE_OFF;
struct PumpConfig {
  float tempThreshold = 30.0f;
  float humiThreshold = 45.0f;
  bool timerEnabled = false;
  int startHour = 6;
  int startMin = 30;
  int endHour = 7;
  int endMin = 0;
};
PumpConfig pumpConfig;
portMUX_TYPE pumpConfigMux = portMUX_INITIALIZER_UNLOCKED;

struct SensorSnapshot {
  float temperature = NAN;
  float humidity = NAN;
};
SensorSnapshot latestSensor;
portMUX_TYPE sensorSnapshotMux = portMUX_INITIALIZER_UNLOCKED;
bool ntpConfigured = false;
volatile bool firebaseModeStreamConnected = false;

PumpConfig pumpConfigSnapshot();
SensorSnapshot sensorSnapshot();
bool isTimerActive(const PumpConfig& config);

unsigned long lastSensorLogTime = 0;
const unsigned long SENSOR_LOG_INTERVAL = 15000; // Ghi nhật ký cảm biến mỗi 15 giây
uint8_t sensorLogSlot = 0;
unsigned long lastStatusPrintTime = 0;
const unsigned long STATUS_PRINT_INTERVAL = 3000;

Preferences firebasePreferences;
String firebaseIdToken;
String firebaseRefreshToken;
unsigned long firebaseTokenRefreshAt = 0;
unsigned long lastFirebaseAuthAttempt = 0;
int lastFirebaseAuthHttpCode = 0;
SemaphoreHandle_t firebaseAuthMutex = nullptr;
volatile bool firebaseAuthRefreshRequested = false;

// ================================================================
// 4. CÁC HÀM XỬ LÝ THỜI GIAN & RƠ-LE
// ================================================================

// Khai báo điều khiển Relay
void setPumpState(bool turnOn) {
  portENTER_CRITICAL(&pumpStateMux);
  if (currentRelayState != turnOn) {
    currentRelayState = turnOn;
    relayStateNeedsPublish = true;
    digitalWrite(PIN_RELAY, turnOn ? RELAY_ON : RELAY_OFF);
  } else {
    digitalWrite(PIN_RELAY, turnOn ? RELAY_ON : RELAY_OFF);
  }
  portEXIT_CRITICAL(&pumpStateMux);
}

void setPumpStateForMode(ControlMode expectedMode, bool turnOn) {
  portENTER_CRITICAL(&pumpStateMux);
  if (currentMode == expectedMode && currentRelayState != turnOn) {
    currentRelayState = turnOn;
    relayStateNeedsPublish = true;
    digitalWrite(PIN_RELAY, turnOn ? RELAY_ON : RELAY_OFF);
  }
  portEXIT_CRITICAL(&pumpStateMux);
}

bool pumpStateSnapshot() {
  portENTER_CRITICAL(&pumpStateMux);
  const bool state = currentRelayState;
  portEXIT_CRITICAL(&pumpStateMux);
  return state;
}

bool pumpStatePublishPending() {
  portENTER_CRITICAL(&pumpStateMux);
  const bool pending = relayStateNeedsPublish;
  portEXIT_CRITICAL(&pumpStateMux);
  return pending;
}

void clearPumpStatePublishPendingIfUnchanged(bool publishedState) {
  portENTER_CRITICAL(&pumpStateMux);
  if (currentRelayState == publishedState) relayStateNeedsPublish = false;
  portEXIT_CRITICAL(&pumpStateMux);
}

void setLocalControlMode(ControlMode nextMode) {
  portENTER_CRITICAL(&pumpStateMux);
  currentMode = nextMode;
  portEXIT_CRITICAL(&pumpStateMux);
}

PumpConfig pumpConfigSnapshot() {
  portENTER_CRITICAL(&pumpConfigMux);
  const PumpConfig snapshot = pumpConfig;
  portEXIT_CRITICAL(&pumpConfigMux);
  return snapshot;
}

SensorSnapshot sensorSnapshot() {
  portENTER_CRITICAL(&sensorSnapshotMux);
  const SensorSnapshot snapshot = latestSensor;
  portEXIT_CRITICAL(&sensorSnapshotMux);
  return snapshot;
}

const char* controlModeName(ControlMode selectedMode) {
  switch (selectedMode) {
    case MODE_AUTO: return "auto";
    case MODE_MANUAL: return "manual";
    default: return "off";
  }
}

void setControlModeFromCloud(const char* modeValue) {
  ControlMode nextMode = MODE_OFF;
  if (modeValue && strcmp(modeValue, "manual") == 0) {
    nextMode = MODE_MANUAL;
  } else if (modeValue && strcmp(modeValue, "auto") == 0) {
    nextMode = MODE_AUTO;
  }

  bool modeChanged = false;
  bool relayCommandApplied = false;
  bool relayTargetState = false;
  portENTER_CRITICAL(&pumpStateMux);
  if (currentMode != nextMode) {
    currentMode = nextMode;
    modeChanged = true;
  }
  if (nextMode == MODE_MANUAL) {
    relayTargetState = true;
    if (currentRelayState != relayTargetState) relayStateNeedsPublish = true;
    currentRelayState = relayTargetState;
    digitalWrite(PIN_RELAY, RELAY_ON);
    relayCommandApplied = true;
  } else if (nextMode == MODE_OFF) {
    relayTargetState = false;
    if (currentRelayState != relayTargetState) relayStateNeedsPublish = true;
    currentRelayState = relayTargetState;
    digitalWrite(PIN_RELAY, RELAY_OFF);
    relayCommandApplied = true;
  }
  portEXIT_CRITICAL(&pumpStateMux);

  if (relayCommandApplied) {
    Serial.printf("[Relay] D18=%s theo lệnh %s.\n",
                  relayTargetState ? "HIGH (BẬT)" : "LOW (TẮT)",
                  controlModeName(nextMode));
  }
  if (modeChanged) {
    Serial.printf("[Firebase SSE] Nhận lệnh chế độ: %s\n", controlModeName(nextMode));
  }
}

// Chạy điều khiển tại chỗ để hệ thống vẫn hoạt động khi mất Wi-Fi.
void updatePumpState(float currentTemp, float currentHumi) {
  bool shouldPumpRun = false;

  const ControlMode selectedMode = currentMode;
  const PumpConfig config = pumpConfigSnapshot();
  if (selectedMode == MODE_MANUAL) {
    shouldPumpRun = true;
  } else if (selectedMode == MODE_AUTO) {
    // Nếu cảm biến lỗi thì tắt bơm an toàn, không coi giá trị 0 là dữ liệu hợp lệ.
    if (!isnan(currentTemp) && !isnan(currentHumi)) {
      bool inTimerWindow = isTimerActive(config);
      bool thresholdMet = (currentTemp >= config.tempThreshold) ||
                          (currentHumi <= config.humiThreshold);
      shouldPumpRun = inTimerWindow || thresholdMet;
    }
  }

  setPumpStateForMode(selectedMode, shouldPumpRun);
}

// Kiểm tra xem giờ hiện tại có nằm trong khoảng hẹn giờ không
bool isTimerActive(const PumpConfig& config) {
  if (!config.timerEnabled) return false;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return false;

  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startMinutes = config.startHour * 60 + config.startMin;
  int endMinutes = config.endHour * 60 + config.endMin;

  if (startMinutes == endMinutes) return false;
  if (startMinutes < endMinutes) {
    return (currentMinutes >= startMinutes && currentMinutes < endMinutes);
  } else {
    // Trường hợp hẹn giờ qua đêm (ví dụ 23:00 -> 01:00)
    return (currentMinutes >= startMinutes || currentMinutes < endMinutes);
  }
}

// ================================================================
// 5. FIREBASE AUTH + REALTIME DATABASE (HTTPS REST API)
// ================================================================

bool isFirebaseConfigured() {
  return String(app_secrets::FIREBASE_API_KEY).length() > 0 &&
         !String(app_secrets::FIREBASE_API_KEY).startsWith("YOUR_") &&
         String(app_secrets::FIREBASE_DATABASE_URL).length() > 0 &&
         !String(app_secrets::FIREBASE_DATABASE_URL).startsWith("https://YOUR_");
}

String encodeFormValue(const String& value) {
  const char hex[] = "0123456789ABCDEF";
  String encoded;
  for (size_t i = 0; i < value.length(); i++) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (safe) {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

int firebaseAuthPost(const String& url, const String& contentType,
                     const String& body, String& response) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if (!http.begin(client, url)) {
    lastFirebaseAuthHttpCode = HTTPC_ERROR_CONNECTION_REFUSED;
    Serial.println("[Firebase Auth] Không khởi tạo được HTTPS request.");
    return lastFirebaseAuthHttpCode;
  }

  http.addHeader("Content-Type", contentType);
  const int httpCode = http.POST(body);
  response = http.getString();
  lastFirebaseAuthHttpCode = httpCode;
  if (httpCode < 200 || httpCode >= 300) {
    Serial.printf("[Firebase Auth] Lỗi HTTP %d (%s): %s\n", httpCode,
                  http.errorToString(httpCode).c_str(), response.c_str());
  }
  http.end();
  return httpCode;
}

bool parseFirebaseTokenResponse(const String& response, bool isRefreshResponse) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.printf("[Firebase Auth] JSON token không hợp lệ: %s\n", error.c_str());
    return false;
  }

  if (isRefreshResponse) {
    firebaseIdToken = doc["id_token"].as<String>();
    firebaseRefreshToken = doc["refresh_token"].as<String>();
  } else {
    firebaseIdToken = doc["idToken"].as<String>();
    firebaseRefreshToken = doc["refreshToken"].as<String>();
  }

  if (firebaseIdToken.length() == 0 || firebaseRefreshToken.length() == 0) {
    Serial.println("[Firebase Auth] Phản hồi thiếu ID token hoặc refresh token.");
    return false;
  }

  const char* expiresField = isRefreshResponse ? "expires_in" : "expiresIn";
  long expiresInSeconds = String(doc[expiresField] | "3600").toInt();
  if (expiresInSeconds <= 60) expiresInSeconds = 3600;
  firebaseTokenRefreshAt = millis() + static_cast<unsigned long>(expiresInSeconds - 60) * 1000UL;
  firebasePreferences.putString("refresh", firebaseRefreshToken);
  return true;
}

bool signInFirebaseAnonymously() {
  StaticJsonDocument<64> requestDoc;
  requestDoc["returnSecureToken"] = true;
  String requestBody;
  serializeJson(requestDoc, requestBody);

  const String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=") +
                     app_secrets::FIREBASE_API_KEY;
  String response;
  const int code = firebaseAuthPost(url, "application/json", requestBody, response);
  if (code != HTTP_CODE_OK || !parseFirebaseTokenResponse(response, false)) return false;

  Serial.println("[Firebase Auth] Đăng nhập ẩn danh thành công.");
  return true;
}

bool refreshFirebaseIdToken() {
  if (firebaseRefreshToken.length() == 0) return false;

  const String url = String("https://securetoken.googleapis.com/v1/token?key=") +
                     app_secrets::FIREBASE_API_KEY;
  const String body = String("grant_type=refresh_token&refresh_token=") +
                      encodeFormValue(firebaseRefreshToken);
  String response;
  const int code = firebaseAuthPost(url, "application/x-www-form-urlencoded", body, response);
  if (code == HTTP_CODE_OK) {
    return parseFirebaseTokenResponse(response, true);
  }

  // Token refresh bị từ chối: bỏ token cũ để lần thử tiếp theo đăng nhập lại.
  if (code == 400 || code == 401) {
    firebaseRefreshToken = "";
    firebasePreferences.remove("refresh");
  }
  return false;
}

bool ensureFirebaseAuth() {
  if (!isFirebaseConfigured()) {
    static bool printedConfigMessage = false;
    if (!printedConfigMessage) {
      Serial.println("[Firebase] Hãy cấu hình include/secrets.h từ include/secrets.example.h.");
      printedConfigMessage = true;
    }
    return false;
  }

  if (firebaseAuthMutex && xSemaphoreTake(firebaseAuthMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return false;
  }

  const bool forceRefresh = firebaseAuthRefreshRequested;
  firebaseAuthRefreshRequested = false;
  if (forceRefresh) firebaseTokenRefreshAt = 0;

  bool authenticated = false;
  if (firebaseIdToken.length() > 0 && static_cast<int32_t>(firebaseTokenRefreshAt - millis()) > 0) {
    authenticated = true;
  } else if (!forceRefresh && lastFirebaseAuthAttempt != 0 &&
             millis() - lastFirebaseAuthAttempt < 15000) {
    authenticated = false;
  } else {
    lastFirebaseAuthAttempt = millis();

    if (firebaseRefreshToken.length() > 0 && refreshFirebaseIdToken()) {
      authenticated = true;
    } else if (firebaseRefreshToken.length() == 0 ||
               (lastFirebaseAuthHttpCode >= 0 && lastFirebaseAuthHttpCode < 500)) {
      authenticated = signInFirebaseAnonymously();
    }
  }

  if (firebaseAuthMutex) xSemaphoreGive(firebaseAuthMutex);
  return authenticated;
}

String firebaseIdTokenSnapshot() {
  if (firebaseAuthMutex && xSemaphoreTake(firebaseAuthMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return "";
  }
  String token = firebaseIdToken;
  if (firebaseAuthMutex) xSemaphoreGive(firebaseAuthMutex);
  return token;
}

String firebaseDatabaseRequestUrl(const String& path) {
  String baseUrl = app_secrets::FIREBASE_DATABASE_URL;
  while (baseUrl.endsWith("/")) baseUrl.remove(baseUrl.length() - 1);
  return baseUrl + "/" + path + ".json?auth=" + firebaseIdToken;
}

int firebaseDatabaseRequest(const char* method, const String& path,
                            const String& body, String& response) {
  if (WiFi.status() != WL_CONNECTED || !ensureFirebaseAuth()) return HTTPC_ERROR_NOT_CONNECTED;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  const String url = firebaseDatabaseRequestUrl(path);
  if (!http.begin(client, url)) {
    Serial.printf("[Firebase] Không khởi tạo được request %s /%s.\n", method, path.c_str());
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }

  http.addHeader("Content-Type", "application/json");
  int httpCode;
  if (strcmp(method, "GET") == 0) {
    httpCode = http.GET();
  } else if (strcmp(method, "PATCH") == 0) {
    httpCode = http.PATCH(body);
  } else if (strcmp(method, "PUT") == 0) {
    httpCode = http.PUT(body);
  } else {
    http.end();
    Serial.printf("[Firebase] HTTP method không hỗ trợ: %s\n", method);
    return HTTPC_ERROR_ENCODING;
  }

  response = http.getString();
  if (httpCode < 200 || httpCode >= 300) {
    Serial.printf("[Firebase] %s /%s lỗi HTTP %d (%s): %s\n", method,
                  path.c_str(), httpCode, http.errorToString(httpCode).c_str(), response.c_str());
  }
  http.end();
  return httpCode;
}

void applyFirebaseConfig(const String& payload) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);
  if (error || !doc.is<JsonObject>()) {
    if (error) Serial.printf("[Firebase] JSON device_state không hợp lệ: %s\n", error.c_str());
    return;
  }

  JsonObject obj = doc.as<JsonObject>();
  if (!firebaseModeStreamConnected) {
    setControlModeFromCloud(obj["mode"] | "off");
  }
  PumpConfig config = pumpConfigSnapshot();
  config.tempThreshold = obj["temp_threshold"] | 30.0f;
  config.humiThreshold = obj["humi_threshold"] | 45.0f;
  config.timerEnabled = obj["timer_enabled"] | false;

  String startTimeStr = obj["start_time"] | "06:30";
  String endTimeStr = obj["end_time"] | "07:00";
  int startColon = startTimeStr.indexOf(':');
  int endColon = endTimeStr.indexOf(':');
  if (startColon > 0 && endColon > 0) {
    config.startHour = startTimeStr.substring(0, startColon).toInt();
    config.startMin = startTimeStr.substring(startColon + 1).toInt();
    config.endHour = endTimeStr.substring(0, endColon).toInt();
    config.endMin = endTimeStr.substring(endColon + 1).toInt();
  }

  portENTER_CRITICAL(&pumpConfigMux);
  pumpConfig = config;
  portEXIT_CRITICAL(&pumpConfigMux);
}

// Tải cấu hình ít thường xuyên hơn; lệnh mode được nhận riêng qua SSE.
void syncDeviceConfigFromCloud() {
  if (WiFi.status() != WL_CONNECTED || !ensureFirebaseAuth()) return;

  String payload;
  const int getCode = firebaseDatabaseRequest("GET", "device_state", "", payload);
  if (getCode >= 200 && getCode < 300 && payload != "null") {
    applyFirebaseConfig(payload);
  }
}

// Gửi relay thực tế và heartbeat, không tham gia vào đường nhận lệnh bơm.
void publishDeviceHeartbeat() {
  if (WiFi.status() != WL_CONNECTED || !ensureFirebaseAuth()) return;
  const bool relayState = pumpStateSnapshot();
  StaticJsonDocument<192> patchDoc;
  patchDoc["relay_on"] = relayState;
  JsonObject timestamp = patchDoc["esp_last_seen"].to<JsonObject>();
  timestamp[".sv"] = "timestamp";

  String patchJson;
  serializeJson(patchDoc, patchJson);
  String response;
  const int httpCode = firebaseDatabaseRequest("PATCH", "device_state", patchJson, response);
  if (httpCode >= 200 && httpCode < 300) {
    clearPumpStatePublishPendingIfUnchanged(relayState);
  }
}

void publishRelayStateIfNeeded() {
  if (WiFi.status() != WL_CONNECTED || !pumpStatePublishPending() || !ensureFirebaseAuth()) return;

  const bool relayState = pumpStateSnapshot();
  StaticJsonDocument<64> patchDoc;
  patchDoc["relay_on"] = relayState;
  String patchJson;
  serializeJson(patchDoc, patchJson);

  String response;
  const int httpCode = firebaseDatabaseRequest("PATCH", "device_state", patchJson, response);
  if (httpCode >= 200 && httpCode < 300) {
    clearPumpStatePublishPendingIfUnchanged(relayState);
  }
}

struct FirebaseSseParser {
  char line[768] = {};
  size_t lineLength = 0;
  bool lineOverflow = false;
  char eventName[24] = {};
  char data[512] = {};
  size_t dataLength = 0;
};

bool dispatchFirebaseSseEvent(FirebaseSseParser& parser) {
  if (strcmp(parser.eventName, "auth_revoked") == 0) {
    firebaseAuthRefreshRequested = true;
    Serial.println("[Firebase SSE] Token bị từ chối; đang yêu cầu làm mới.");
    return false;
  }
  if (strcmp(parser.eventName, "cancel") == 0) {
    Serial.println("[Firebase SSE] Luồng bị từ chối bởi Realtime Database Rules.");
    return false;
  }

  if ((strcmp(parser.eventName, "put") == 0 || strcmp(parser.eventName, "patch") == 0) &&
      parser.dataLength > 0) {
    JsonDocument eventDoc;
    DeserializationError error = deserializeJson(eventDoc, parser.data, parser.dataLength);
    if (!error) {
      const char* path = eventDoc["path"] | "/";
      JsonVariant value = eventDoc["data"];
      if (strcmp(path, "/") == 0 || path[0] == '\0') {
        if (value.isNull()) {
          setControlModeFromCloud("off");
          firebaseModeStreamConnected = true;
        } else if (value.is<const char*>()) {
          setControlModeFromCloud(value.as<const char*>());
          firebaseModeStreamConnected = true;
        }
      }
    }
  }

  return true;
}

bool processFirebaseSseLine(FirebaseSseParser& parser) {
  if (parser.lineOverflow) {
    parser.lineOverflow = false;
    parser.lineLength = 0;
    return true;
  }
  parser.line[parser.lineLength] = '\0';

  if (parser.lineLength == 0) {
    const bool keepStreaming = dispatchFirebaseSseEvent(parser);
    parser.eventName[0] = '\0';
    parser.dataLength = 0;
    parser.data[0] = '\0';
    return keepStreaming;
  }

  if (parser.line[0] != ':') {
    char* value = strchr(parser.line, ':');
    if (value) {
      *value++ = '\0';
      if (*value == ' ') value++;
    } else {
      value = parser.line + strlen(parser.line);
    }

    if (strcmp(parser.line, "event") == 0) {
      strlcpy(parser.eventName, value, sizeof(parser.eventName));
    } else if (strcmp(parser.line, "data") == 0) {
      const size_t valueLength = strlen(value);
      const size_t separatorLength = parser.dataLength > 0 ? 1 : 0;
      if (parser.dataLength + separatorLength + valueLength < sizeof(parser.data)) {
        if (separatorLength) parser.data[parser.dataLength++] = '\n';
        memcpy(parser.data + parser.dataLength, value, valueLength);
        parser.dataLength += valueLength;
        parser.data[parser.dataLength] = '\0';
      }
    }
  }

  parser.lineLength = 0;
  return true;
}

bool feedFirebaseSseByte(FirebaseSseParser& parser, uint8_t value) {
  if (value == '\r') return true;
  if (value == '\n') return processFirebaseSseLine(parser);

  if (parser.lineLength + 1 < sizeof(parser.line)) {
    parser.line[parser.lineLength++] = static_cast<char>(value);
  } else {
    parser.lineOverflow = true;
  }
  return true;
}

enum ChunkReadState : uint8_t { CHUNK_SIZE_LINE, CHUNK_DATA_BYTES, CHUNK_DATA_CR,
                                CHUNK_DATA_LF, CHUNK_FINISHED };

struct FirebaseChunkDecoder {
  ChunkReadState state = CHUNK_SIZE_LINE;
  char sizeLine[24] = {};
  size_t sizeLineLength = 0;
  uint32_t remaining = 0;
};

bool feedFirebaseChunkByte(FirebaseChunkDecoder& decoder, FirebaseSseParser& parser,
                           uint8_t value) {
  switch (decoder.state) {
    case CHUNK_SIZE_LINE:
      if (value == '\r') return true;
      if (value == '\n') {
        decoder.sizeLine[decoder.sizeLineLength] = '\0';
        char* extension = strchr(decoder.sizeLine, ';');
        if (extension) *extension = '\0';
        decoder.remaining = static_cast<uint32_t>(strtoul(decoder.sizeLine, nullptr, 16));
        decoder.sizeLineLength = 0;
        if (decoder.remaining == 0) {
          decoder.state = CHUNK_FINISHED;
          return false;
        }
        decoder.state = CHUNK_DATA_BYTES;
        return true;
      }
      if (decoder.sizeLineLength + 1 < sizeof(decoder.sizeLine)) {
        decoder.sizeLine[decoder.sizeLineLength++] = static_cast<char>(value);
        return true;
      }
      return false;

    case CHUNK_DATA_BYTES:
      if (!feedFirebaseSseByte(parser, value)) return false;
      if (--decoder.remaining == 0) decoder.state = CHUNK_DATA_CR;
      return true;

    case CHUNK_DATA_CR:
      if (value != '\r') return false;
      decoder.state = CHUNK_DATA_LF;
      return true;

    case CHUNK_DATA_LF:
      if (value != '\n') return false;
      decoder.state = CHUNK_SIZE_LINE;
      return true;

    case CHUNK_FINISHED:
    default:
      return false;
  }
}

void firebaseModeStreamTask(void* parameter) {
  (void)parameter;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    const String token = firebaseIdTokenSnapshot();
    if (token.length() == 0) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    String baseUrl = app_secrets::FIREBASE_DATABASE_URL;
    while (baseUrl.endsWith("/")) baseUrl.remove(baseUrl.length() - 1);
    const String streamUrl = baseUrl + "/device_state/mode.json?auth=" + token;

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(10000);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    const char* responseHeaders[] = {"Transfer-Encoding"};
    http.collectHeaders(responseHeaders, 1);

    if (!http.begin(client, streamUrl)) {
      Serial.println("[Firebase SSE] Không mở được kết nối lệnh.");
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    http.addHeader("Accept", "text/event-stream");
    http.addHeader("Cache-Control", "no-cache");
    const int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
      firebaseModeStreamConnected = false;
      if (httpCode == HTTP_CODE_UNAUTHORIZED || httpCode == HTTP_CODE_FORBIDDEN) {
        firebaseAuthRefreshRequested = true;
      }
      Serial.printf("[Firebase SSE] Mở luồng lỗi HTTP %d (%s).\n",
                    httpCode, http.errorToString(httpCode).c_str());
      http.end();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    const bool chunked = http.header("Transfer-Encoding").indexOf("chunked") >= 0;
    Serial.printf("[Firebase SSE] Đã kết nối luồng điều khiển (%s).\n",
                  chunked ? "chunked" : "identity");

    FirebaseSseParser parser;
    FirebaseChunkDecoder decoder;
    NetworkClient* stream = http.getStreamPtr();
    unsigned long lastStreamActivity = millis();

    while (WiFi.status() == WL_CONNECTED && stream) {
      bool reconnect = false;
      while (stream->available() > 0) {
        const int value = stream->read();
        if (value < 0) break;
        lastStreamActivity = millis();
        const bool keepStreaming = chunked
            ? feedFirebaseChunkByte(decoder, parser, static_cast<uint8_t>(value))
            : feedFirebaseSseByte(parser, static_cast<uint8_t>(value));
        if (!keepStreaming) {
          reconnect = true;
          break;
        }
      }

      if (reconnect || (!stream->connected() && stream->available() == 0)) break;
      if (millis() - lastStreamActivity > 90000UL) {
        Serial.println("[Firebase SSE] Luồng quá thời gian chờ; đang kết nối lại.");
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }

    http.end();
    firebaseModeStreamConnected = false;
    if (WiFi.status() == WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// Ghi log vào 10 ô luân phiên để database không tăng vô hạn.
void sendSensorLogToCloud(float currentTemp, float currentHumi) {
  if (WiFi.status() != WL_CONNECTED || isnan(currentTemp) || isnan(currentHumi)) return;
  if (!ensureFirebaseAuth()) return;

  StaticJsonDocument<192> logDoc;
  logDoc["temperature"] = currentTemp;
  logDoc["humidity"] = currentHumi;
  logDoc["relay_on"] = pumpStateSnapshot();
  JsonObject timestamp = logDoc["created_at"].to<JsonObject>();
  timestamp[".sv"] = "timestamp";

  String logJson;
  serializeJson(logDoc, logJson);
  const String logPath = String("sensor_logs/") + String(sensorLogSlot);
  String response;
  const int httpCode = firebaseDatabaseRequest("PUT", logPath, logJson, response);
  if (httpCode >= 200 && httpCode < 300) {
    Serial.printf("[Firebase] Ghi sensor_logs/%u thành công (HTTP %d).\n",
                  static_cast<unsigned>(sensorLogSlot), httpCode);
    sensorLogSlot = (sensorLogSlot + 1) % 10;
  }
}

// Các request HTTPS định kỳ chạy riêng để không chặn vòng lặp điều khiển relay.
void firebaseMaintenanceTask(void* parameter) {
  (void)parameter;
  const unsigned long configInterval = 15000;
  const unsigned long heartbeatInterval = 30000;
  bool didConfigSync = false;
  bool didHeartbeat = false;
  unsigned long lastConfigSync = 0;
  unsigned long lastHeartbeat = 0;
  unsigned long lastRelayPublishAttempt = 0;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      const unsigned long now = millis();
      if (pumpStatePublishPending() && now - lastRelayPublishAttempt >= 1000UL) {
        lastRelayPublishAttempt = now;
        publishRelayStateIfNeeded();
      }

      if (!didConfigSync || now - lastConfigSync >= configInterval) {
        didConfigSync = true;
        lastConfigSync = now;
        syncDeviceConfigFromCloud();
      }

      if (!didHeartbeat || millis() - lastHeartbeat >= heartbeatInterval) {
        didHeartbeat = true;
        lastHeartbeat = millis();
        publishDeviceHeartbeat();
      }

      if (millis() - lastSensorLogTime >= SENSOR_LOG_INTERVAL) {
        lastSensorLogTime = millis();
        const SensorSnapshot latest = sensorSnapshot();
        sendSensorLogToCloud(latest.temperature, latest.humidity);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ================================================================
// 6. KHỞI TẠO HỆ THỐNG (SETUP)
// ================================================================
void setup() {
  Serial.begin(9600);
  delay(500);
  Serial.println("\n========== NÔNG TRẠI THÔNG MINH IoT ==========");

  firebaseAuthMutex = xSemaphoreCreateMutex();
  if (!firebaseAuthMutex) {
    Serial.println("[Firebase Auth] Không tạo được mutex bảo vệ token.");
  }

  firebasePreferences.begin("plant-cloud", false);
  firebaseRefreshToken = firebasePreferences.getString("refresh", "");

  // Kéo chân D18 xuống LOW ngay lập tức khi khởi động để tránh nháy máy bơm
  pinMode(PIN_RELAY, OUTPUT);
  setPumpState(false);
  portENTER_CRITICAL(&pumpStateMux);
  relayStateNeedsPublish = true;
  portEXIT_CRITICAL(&pumpStateMux);

  // Khởi tạo cảm biến DHT11 ở chân D4 (GPIO 4)
  dht.setup(PIN_DHT11, DHTesp::DHT11);
  Serial.println("[Hardware] Đã khởi tạo DHT11 (D4) và Relay (D18).");

  // Kết nối Wi-Fi
  Serial.printf("[WiFi] Đang kết nối tới %s ", app_secrets::WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(app_secrets::WIFI_SSID, app_secrets::WIFI_PASSWORD);

  int wifiRetry = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetry < 30) {
    delay(500);
    Serial.print(".");
    wifiRetry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Kết nối Wi-Fi thành công!");
    Serial.print("[WiFi] IP ESP32: ");
    Serial.println(WiFi.localIP());

    ensureFirebaseAuth();

    // Cấu hình lấy giờ chuẩn NTP tự động qua Internet
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    ntpConfigured = true;
    Serial.println("[NTP] Đang đồng bộ thời gian thực...");

  } else {
    Serial.println("\n[WiFi] Kết nối Wi-Fi thất bại! Mạch sẽ tự kết nối lại trong loop.");
  }

  if (firebaseAuthMutex) {
    const BaseType_t streamTaskCreated = xTaskCreatePinnedToCore(
      firebaseModeStreamTask, "firebase-mode", 12288, nullptr, 1, nullptr, 0);
    if (streamTaskCreated == pdPASS) {
      Serial.println("[Firebase SSE] Đã khởi chạy luồng nhận lệnh bơm realtime.");
    } else {
      Serial.println("[Firebase SSE] Không tạo được task nhận lệnh realtime.");
    }
  } else {
    Serial.println("[Firebase SSE] Đã bỏ qua luồng realtime vì thiếu mutex token.");
  }

  const BaseType_t maintenanceTaskCreated = xTaskCreatePinnedToCore(
    firebaseMaintenanceTask, "firebase-maint", 8192, nullptr, 1, nullptr, 0);
  if (maintenanceTaskCreated != pdPASS) {
    Serial.println("[Firebase] Không tạo được task gửi dữ liệu định kỳ.");
  }
}

// ================================================================
// 7. VÒNG LẶP CHÍNH (LOOP)
// ================================================================
void loop() {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;

  // Nếu Wi-Fi kết nối lại sau khi khởi động offline, bật đồng bộ giờ NTP.
  if (wifiConnected && !ntpConfigured) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    ntpConfigured = true;
    Serial.println("[NTP] Đang đồng bộ thời gian sau khi Wi-Fi kết nối lại...");
  }

  // Tự động kết nối lại định kỳ nhưng vẫn tiếp tục điều khiển tại chỗ.
  if (!wifiConnected) {
    static unsigned long lastWiFiReconnect = 0;
    if (millis() - lastWiFiReconnect > 10000) {
      lastWiFiReconnect = millis();
      Serial.println("[WiFi] Mất kết nối, đang thử kết nối lại...");
      WiFi.reconnect();
    }
    // Khi offline, bỏ qua cấu hình cloud và chạy theo ngưỡng tự động đã lưu.
    setLocalControlMode(MODE_AUTO);
  }

  // 1. Đọc dữ liệu từ DHT11
  TempAndHumidity data = dht.getTempAndHumidity();
  float temp = data.temperature;
  float humi = data.humidity;

  if (dht.getStatus() != DHTesp::ERROR_NONE || isnan(temp) || isnan(humi)) {
    temp = NAN;
    humi = NAN;
    Serial.println("[DHT11] Đọc cảm biến lỗi; chế độ tự động sẽ giữ bơm tắt.");
  }

  portENTER_CRITICAL(&sensorSnapshotMux);
  latestSensor.temperature = temp;
  latestSensor.humidity = humi;
  portEXIT_CRITICAL(&sensorSnapshotMux);

  // 2. Điều khiển relay tại chỗ trong mỗi vòng lặp (~100 ms).
  // Lệnh mode mới được task SSE cập nhật độc lập, không đợi chu kỳ ghi cảm biến.
  updatePumpState(temp, humi);

  const unsigned long now = millis();
  if (now - lastStatusPrintTime >= STATUS_PRINT_INTERVAL) {
    lastStatusPrintTime = now;
    Serial.printf("[Status] Nhiệt độ: %.1f°C | Độ ẩm: %.0f%% | Chế độ: %s | Relay D18: %s\n",
                  temp, humi, controlModeName(currentMode),
                  pumpStateSnapshot() ? "HIGH (BẬT)" : "LOW (TẮT)");
  }

  delay(100);
}
