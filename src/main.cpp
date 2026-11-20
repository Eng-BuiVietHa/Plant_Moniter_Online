#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <time.h>
#include <math.h>

// ================================================================
// 1. CẤU HÌNH KẾT NỐI WI-FI & SUPABASE
// ================================================================
const char* WIFI_SSID = "TEN_WIFI_CUA_BAN";
const char* WIFI_PASSWORD = "MAT_KHAU_WIFI";

// Khóa API và URL dự án Supabase
const char* SUPABASE_URL = "https://YOUR_PROJECT_ID.supabase.co";
const char* SUPABASE_KEY = "YOUR_SUPABASE_ANON_KEY";

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

// Cấu hình đồng bộ từ Supabase
String mode = "off"; // "auto" | "manual" | "off"
float tempThreshold = 30.0; // Ngưỡng nhiệt độ tự động (°C)
float humiThreshold = 60.0; // Ngưỡng độ ẩm tự động (%)
bool timerEnabled = false; // Trạng thái bật/tắt hẹn giờ
int startHour = 6, startMin = 30;
int endHour = 7, endMin = 0;
bool ntpConfigured = false;

bool isTimerActive();

// Bộ đếm thời gian thực thi (Non-blocking)
unsigned long lastStateSyncTime = 0;
const unsigned long STATE_SYNC_INTERVAL = 3000; // Đồng bộ lệnh & gửi nhịp tim mỗi 3 giây

unsigned long lastSensorLogTime = 0;
const unsigned long SENSOR_LOG_INTERVAL = 60000; // Ghi nhật ký cảm biến mỗi 60 giây

// ================================================================
// 4. CÁC HÀM XỬ LÝ THỜI GIAN & RƠ-LE
// ================================================================

// Khai báo điều khiển Relay
void setPumpState(bool turnOn) {
  currentRelayState = turnOn;
  digitalWrite(PIN_RELAY, turnOn ? RELAY_ON : RELAY_OFF);
}

// Chạy điều khiển tại chỗ để hệ thống vẫn hoạt động khi mất Wi-Fi.
void updatePumpState(float currentTemp, float currentHumi) {
  bool shouldPumpRun = false;

  if (mode == "manual") {
    shouldPumpRun = true;
  } else if (mode == "auto") {
    // Nếu cảm biến lỗi thì tắt bơm an toàn, không coi giá trị 0 là dữ liệu hợp lệ.
    if (!isnan(currentTemp) && !isnan(currentHumi)) {
      bool inTimerWindow = isTimerActive();
      bool thresholdMet = (currentTemp >= tempThreshold) || (currentHumi <= humiThreshold);
      shouldPumpRun = inTimerWindow || thresholdMet;
    }
  }

  setPumpState(shouldPumpRun);
}

// Lấy chuỗi thời gian chuẩn ISO 8601 để báo nhịp tim (esp_last_seen)
String getISO8601Timestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "";
  }
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &timeinfo);
  return String(buf);
}

// Kiểm tra xem giờ hiện tại có nằm trong khoảng hẹn giờ không
bool isTimerActive() {
  if (!timerEnabled) return false;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return false;

  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startMinutes = startHour * 60 + startMin;
  int endMinutes = endHour * 60 + endMin;

  if (startMinutes == endMinutes) return false;
  if (startMinutes < endMinutes) {
    return (currentMinutes >= startMinutes && currentMinutes < endMinutes);
  } else {
    // Trường hợp hẹn giờ qua đêm (ví dụ 23:00 -> 01:00)
    return (currentMinutes >= startMinutes || currentMinutes < endMinutes);
  }
}

// ================================================================
// 5. GIAO TIẾP CLOUD SUPABASE (HTTPS REST API)
// ================================================================

// Đồng bộ 2 chiều với Supabase: Lấy cấu hình + Đóng/Ngắt Relay + Báo nhịp tim
void syncDeviceStateWithCloud(float currentTemp, float currentHumi) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure(); // Tối ưu SSL cho ESP32

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/device_state?id=eq.1";

  // --- A. LẤY LỆNH ĐIỀU KHIỂN MỚI TỪ BẢNG device_state (GET) ---
  http.begin(client, url);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, payload);

    if (!err && doc.is<JsonArray>() && doc.as<JsonArray>().size() > 0) {
      JsonObject obj = doc[0];
      mode = obj["mode"].as<String>();
      tempThreshold = obj["temp_threshold"] | 30.0f;
      humiThreshold = obj["humi_threshold"] | 60.0f;
      timerEnabled = obj["timer_enabled"] | false;

      String startTimeStr = obj["start_time"] | "06:30";
      String endTimeStr = obj["end_time"] | "07:00";

      startHour = startTimeStr.substring(0, startTimeStr.indexOf(':')).toInt();
      startMin = startTimeStr.substring(startTimeStr.indexOf(':') + 1).toInt();
      endHour = endTimeStr.substring(0, endTimeStr.indexOf(':')).toInt();
      endMin = endTimeStr.substring(endTimeStr.indexOf(':') + 1).toInt();
    }
  }
  http.end();

  // --- B. ÁP DỤNG LOGIC BẬT/TẮT MÁY BƠM ---
  updatePumpState(currentTemp, currentHumi);

  // --- C. CẬP NHẬT TRẠNG THÁI MÁY BƠM REALTIME & NHỊP TIM (PATCH) ---
  http.begin(client, url);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> patchDoc;
  patchDoc["relay_on"] = currentRelayState;

  String isoTime = getISO8601Timestamp();
  if (isoTime.length() > 0) {
    patchDoc["esp_last_seen"] = isoTime;
  }

  String patchJson;
  serializeJson(patchDoc, patchJson);

  http.PATCH(patchJson);
  http.end();
}

// Ghi số liệu cảm biến vào bảng sensor_logs để vẽ đồ thị (POST)
void sendSensorLogToCloud(float currentTemp, float currentHumi) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (isnan(currentTemp) || isnan(currentHumi)) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(SUPABASE_URL) + "/rest/v1/sensor_logs";

  http.begin(client, url);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");

  StaticJsonDocument<128> logDoc;
  logDoc["temperature"] = currentTemp;
  logDoc["humidity"] = currentHumi;
  logDoc["relay_on"] = currentRelayState;

  String logJson;
  serializeJson(logDoc, logJson);

  int httpCode = http.POST(logJson);
  if (httpCode == 201 || httpCode == 200) {
    Serial.println("[Supabase] Đã ghi nhật ký cảm biến thành công!");
  }
  http.end();
}

// ================================================================
// 6. KHỞI TẠO HỆ THỐNG (SETUP)
// ================================================================
void setup() {
  Serial.begin(9600);
  delay(500);
  Serial.println("\n========== NÔNG TRẠI THÔNG MINH IoT ==========");

  // Kéo chân D18 xuống LOW ngay lập tức khi khởi động để tránh nháy máy bơm
  pinMode(PIN_RELAY, OUTPUT);
  setPumpState(false);

  // Khởi tạo cảm biến DHT11 ở chân D4 (GPIO 4)
  dht.setup(PIN_DHT11, DHTesp::DHT11);
  Serial.println("[Hardware] Đã khởi tạo DHT11 (D4) và Relay (D18).");

  // Kết nối Wi-Fi
  Serial.printf("[WiFi] Đang kết nối tới %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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

    // Cấu hình lấy giờ chuẩn NTP tự động qua Internet
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    ntpConfigured = true;
    Serial.println("[NTP] Đang đồng bộ thời gian thực...");
  } else {
    Serial.println("\n[WiFi] Kết nối Wi-Fi thất bại! Mạch sẽ tự kết nối lại trong loop.");
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
    mode = "auto";
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

  // 2. Đồng bộ lệnh điều khiển + Gửi nhịp tim (mỗi 3 giây)
  if (millis() - lastStateSyncTime >= STATE_SYNC_INTERVAL) {
    lastStateSyncTime = millis();
    if (wifiConnected) {
      syncDeviceStateWithCloud(temp, humi);
    } else {
      updatePumpState(temp, humi);
    }

    Serial.printf("[Status] Nhiệt độ: %.1f°C | Độ ẩm: %.0f%% | Chế độ: %s | Relay D18: %s\n",
                  temp, humi, mode.c_str(), currentRelayState ? "HIGH (BẬT)" : "LOW (TẮT)");
  }

  // 3. Ghi lịch sử cảm biến vào Supabase để vẽ đồ thị (mỗi 60 giây)
  if (wifiConnected && millis() - lastSensorLogTime >= SENSOR_LOG_INTERVAL) {
    lastSensorLogTime = millis();
    sendSensorLogToCloud(temp, humi);
  }

  delay(100);
}
