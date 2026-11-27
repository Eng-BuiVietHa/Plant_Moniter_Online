# Plant Monitor IoT

Hệ thống giám sát nhiệt độ, độ ẩm và điều khiển máy bơm cho cây trồng. ESP32 đọc cảm biến DHT11, điều khiển relay tại chỗ và đồng bộ cấu hình/nhật ký lên Firebase khi có Wi‑Fi. Giao diện web hiển thị số liệu, biểu đồ và cho phép đổi chế độ điều khiển.

## Tính năng

- Đọc nhiệt độ và độ ẩm từ DHT11.
- Điều khiển máy bơm qua relay ở ba chế độ: **Tự động**, **Làm mát/Thủ công**, **Tắt**.
- Chế độ tự động bật bơm khi nhiệt độ đạt ngưỡng, độ ẩm xuống dưới ngưỡng, hoặc đang trong khung giờ hẹn.
- Ngưỡng mặc định: nhiệt độ ≥ 30°C (giữ nguyên), độ ẩm không khí ≤ 45% (đã hạ từ 60% để bơm ít nhạy hơn với độ ẩm).
- Tiếp tục điều khiển tự động khi ESP32 mất Wi‑Fi; khi offline, thiết bị ngừng gửi dữ liệu lên Firebase.
- Nhận lệnh bơm qua Firebase realtime; lưu lịch sử cảm biến khoảng mỗi 15 giây.
- Web app hiển thị biểu đồ 10 lần đo gần nhất và có thể cài như PWA.

## Phần cứng

### Linh kiện

- ESP32 DevKit V1.
- Cảm biến DHT11 (module hoặc cảm biến rời).
- Module relay tương thích tín hiệu điều khiển 3,3 V của ESP32.
- Máy bơm và bộ nguồn phù hợp với điện áp/dòng của máy bơm.

### Sơ đồ nối ESP32, DHT11 và relay

~~~mermaid
flowchart LR
  subgraph ESP32[ESP32 DevKit V1]
    V3[3V3]
    GND[GND]
    D4[GPIO 4]
    D18[GPIO 18]
    VIN[5V / VIN]
  end

  subgraph DHT[DHT11]
    DV[VCC]
    DG[GND]
    DATA[DATA]
  end

  subgraph RELAY[Module relay]
    RV[VCC]
    RG[GND]
    IN[IN]
    COM[COM]
    NO[NO]
  end

  V3 --> DV
  GND --> DG
  GND --> RG
  D4 --> DATA
  D18 --> IN
  VIN -. Nguồn relay nếu module yêu cầu 5 V .-> RV
~~~

- DHT11: VCC nối 3V3, GND nối GND, DATA nối GPIO 4.
- Relay: IN nối GPIO 18, GND nối chung với ESP32. Cấp VCC theo thông số module; chỉ nối VIN/5V nếu relay cần 5 V.
- Firmware cấu hình relay active HIGH. Xác nhận module tương thích mức logic 3,3 V và active HIGH; nhiều module kích mức thấp, cần đổi RELAY_ON/RELAY_OFF trong src/main.cpp nếu dùng loại đó.
- DHT11 rời (không phải module) thường cần điện trở kéo lên DATA–3V3; kiểm tra datasheet của linh kiện.

### Nối máy bơm DC qua tiếp điểm relay

~~~mermaid
flowchart LR
  PSU["+ nguồn máy bơm DC"] --> COM[Relay COM]
  COM -->|Relay đóng| NO[Relay NO]
  NO --> PPLUS["Máy bơm +"]
  PNEG["Máy bơm −"] --> PSU_NEG["− nguồn máy bơm DC"]
~~~

Dùng tiếp điểm COM và NO để máy bơm mặc định tắt khi relay chưa kích. Nguồn máy bơm đi qua tiếp điểm relay; không cấp máy bơm trực tiếp từ chân ESP32. Sơ đồ trên dành cho bơm DC. Nếu dùng điện lưới AC, cần relay có định mức phù hợp, cách điện đúng chuẩn và người có chuyên môn đấu nối.

## Cấu trúc phần mềm

~~~mermaid
flowchart TD
  DHT[DHT11] --> FW[Firmware ESP32: src/main.cpp]
  FW --> RELAY[GPIO 18 / relay / máy bơm]
  FW <-->|REST API: cấu hình, trạng thái, nhật ký| DB[(Firebase)]
  WEB[Web app: web-app] <-->|REST + Realtime| DB
  PWA[manifest.json + sw.js] --> WEB
  FW --> WIFI{Wi‑Fi hoạt động?}
  WIFI -->|Mất| LOCAL[Chạy tự động tại chỗ, không gửi dữ liệu]
  WIFI -->|Có| CLOUD[Đồng bộ Firebase]
~~~

~~~text
Plant_Moniter/
├── platformio.ini       Cấu hình board ESP32 và thư viện
├── src/main.cpp         Cảm biến, điều khiển relay, Wi‑Fi và Firebase
├── web-app/
│   ├── index.html       Bố cục dashboard và hộp cài đặt hẹn giờ
│   ├── style.css        Giao diện web
│   ├── script.js        Kết nối Firebase, điều khiển UI và biểu đồ
│   ├── manifest.json    Thông tin cài đặt PWA
│   ├── sw.js            Cache file tĩnh cho PWA
│   └── icon-*.png       Biểu tượng PWA
├── include/             Header của PlatformIO
├── lib/                 Thư viện riêng của PlatformIO
└── test/                Thư mục kiểm thử PlatformIO
~~~

## Logic điều khiển

- **Tự động (auto):** bật bơm nếu nhiệt độ >= temp_threshold, hoặc độ ẩm <= humi_threshold, hoặc hẹn giờ đang hoạt động.
- **Làm mát/Thủ công (manual):** firmware bật relay liên tục.
- **Tắt (off):** tắt relay.
- **Mất Wi‑Fi:** ESP32 chuyển sang auto, tiếp tục đọc cảm biến/điều khiển tại chỗ và bỏ qua đồng bộ/gửi log cloud. Nếu vừa mất kết nối sau khi tải cấu hình, ngưỡng còn trong RAM. Nếu khởi động offline, dùng ngưỡng mặc định trong firmware (30 °C, 60%) và hẹn giờ mặc định tắt.
- **DHT11 lỗi trong chế độ tự động:** giữ bơm tắt để tránh bật bơm do giá trị cảm biến không hợp lệ.
- Hẹn giờ cần thời gian đúng. ESP32 lấy giờ NTP qua Wi‑Fi; nếu khởi động hoàn toàn offline, lịch có thể chưa hoạt động cho đến khi đồng bộ giờ.

## Cấu hình Firebase Realtime Database

### 1. Tạo project và Realtime Database

1. Tạo project trong Firebase Console và thêm một ứng dụng Web để lấy cấu hình Firebase.
2. Mở **Build → Realtime Database**, tạo database và sao chép đúng **Database URL** mà Console hiển thị.
3. Mở **Build → Authentication → Sign-in method**, bật **Anonymous**.
4. Trong **Realtime Database → Rules**, dùng rules sau cho prototype:

~~~json
{
  "rules": {
    ".read": "auth != null",
    ".write": "auth != null",
    "sensor_logs": {
      ".indexOn": ["created_at"]
    }
  }
}
~~~

Ứng dụng web và ESP32 đăng nhập ẩn danh trước khi truy cập database. Rule `auth != null` cho phép mọi tài khoản Firebase đã xác thực, kể cả tài khoản ẩn danh, đọc/ghi database; phù hợp thử nghiệm cá nhân nhưng chưa giới hạn theo từng thiết bị khi triển khai công khai.

### 2. Cấu trúc dữ liệu

Realtime Database lưu dữ liệu dạng cây JSON, nên không cần tạo bảng hay schema như SQL. Web tự tạo `device_state` cùng các giá trị mặc định khi kết nối lần đầu. ESP32 tạo các node `sensor_logs/0` đến `sensor_logs/9` khi gửi log cảm biến hợp lệ; sau đó cập nhật dữ liệu khoảng mỗi 15 giây. Firmware luân phiên 10 vị trí để dữ liệu không tăng vô hạn.

### 3. Điền cấu hình Firebase

- Firmware: sao chép `include/secrets.example.h` thành `include/secrets.h`, rồi điền Wi-Fi, Firebase Web API Key và Realtime Database URL.
- Web: cấu hình `apiKey`, `authDomain`, `databaseURL`, `projectId` và `appId` trong `web-app/firebase-config.js`. File này sẽ nằm trong repo riêng tư cùng project.
- Firebase Web API key và các trường cấu hình client vẫn được gửi tới trình duyệt khi chạy web. Quyền truy cập dữ liệu phải được bảo vệ bằng Realtime Database Rules. Không đặt mật khẩu Wi-Fi hoặc service account/private key trong cấu hình web. Dùng cùng một Firebase project cho firmware và web.

Firmware gọi Firebase Auth REST để đăng nhập ẩn danh và tự làm mới token. Token làm mới được lưu trong NVS của ESP32.

## Cài đặt và chạy

### Nạp firmware lên ESP32

1. Cài Visual Studio Code và extension PlatformIO IDE.
2. Mở thư mục gốc Plant_Moniter.
3. Tạo `include/secrets.h` từ file mẫu và điền thông tin Wi-Fi/Firebase của bạn.
4. Kết nối ESP32 qua USB và chọn đúng cổng serial.
5. Chọn PlatformIO Build, sau đó Upload.
6. Mở Serial Monitor ở 9600 baud.

Pin được cấu hình trong code: DHT11 ở GPIO 4, relay ở GPIO 18.

### Chạy web app

1. Điền cấu hình ứng dụng Web Firebase trong `web-app/firebase-config.js`; nếu thiếu file, sao chép `firebase-config.example.js` vào `web-app` trước.
2. Phục vụ thư mục web-app qua máy chủ web cục bộ (ví dụ Live Server) hoặc deploy HTTPS.
3. Mở trang trong trình duyệt. Để cài PWA, chọn Install app/Add to Home Screen nếu trình duyệt hỗ trợ.
4. Đăng nhập ẩn danh phải được bật trong Firebase Authentication; RTDB sẽ tự gửi cập nhật realtime cho web.

### Deploy web app lên Vercel

1. Import repo GitHub riêng tư vào Vercel.
2. Đặt **Root Directory** là `web-app` và **Framework Preset** là `Other`.
3. Để trống Build Command; thư mục này là web tĩnh và có `index.html` ở gốc.

Không mở trực tiếp bằng file://; service worker cần localhost hoặc HTTPS. PWA có thể mở giao diện đã cache offline nhưng không thể đọc/ghi Firebase khi mất mạng.

## Cách sử dụng

- Chọn **Tự động** để điều khiển theo ngưỡng và lịch.
- Chọn **Làm mát** để bật bơm thủ công.
- Chọn **Tắt** để tắt bơm.
- Chọn **Hẹn giờ** để đặt giờ bắt đầu/kết thúc hằng ngày; hẹn giờ chỉ tác động ở chế độ tự động.
- Dashboard hiển thị nhiệt độ, độ ẩm, trạng thái kết nối Firebase, relay và tối đa 10 bản ghi gần nhất.
- ESP32 giữ một luồng Firebase realtime riêng cho lệnh chế độ bơm; ở chế độ thủ công, relay được đổi ngay khi nhận lệnh. Cảm biến vẫn được đọc tại chỗ để điều khiển tự động.
- Lịch sử cảm biến được ghi khoảng mỗi 15 giây; cấu hình được tải lại khoảng mỗi 15 giây và heartbeat khoảng mỗi 30 giây. Trạng thái relay thực tế được gửi riêng ngay sau khi relay đổi; độ trễ hiển thị còn phụ thuộc mạng.
- Khi Wi‑Fi của ESP32 mất, thiết bị tự chạy chế độ tự động tại chỗ và ngừng gửi dữ liệu cloud. Khi mạng trở lại, ESP32 thử kết nối lại.

## Xử lý sự cố

| Hiện tượng | Kiểm tra |
| --- | --- |
| Web không đọc được dữ liệu | Database URL/Web API Key, Anonymous Auth và Realtime Database Rules |
| Biểu đồ trống | DHT11 có đọc hợp lệ không; kiểm tra node sensor_logs và chờ lần ghi đầu tiên |
| Bấm nút nhưng relay không đổi | Kiểm tra Anonymous Auth/Rules và Serial Monitor có dòng `[Firebase SSE] Đã kết nối luồng điều khiển` không |
| Web không cập nhật tức thời | Kiểm tra listener onValue và lỗi trong Console của trình duyệt |
| Relay hoạt động ngược | Kiểm tra module active HIGH/LOW và RELAY_ON/RELAY_OFF |
| Hẹn giờ không chạy | Kiểm tra timer_enabled, khung giờ và NTP |
| Bơm không chạy tự động | Kiểm tra mode, ngưỡng, DHT11 và relay |

## Giới hạn hiện tại

- Khi ESP32 khởi động offline, ngưỡng chỉ dùng giá trị mặc định; cấu hình Firebase được tải khi kết nối lại.
- Lịch hẹn giờ cần đồng hồ hệ thống đã đồng bộ.
- Firebase Web API Key là thông tin cấu hình công khai; Security Rules vẫn cần giới hạn quyền phù hợp.
- Web và ESP32 hiện đăng nhập ẩn danh; chưa giới hạn quyền theo UID của từng người dùng hoặc thiết bị.
