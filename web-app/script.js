// ============================================================
// 1. CẤU HÌNH KẾT NỐI SUPABASE (Điền thông tin Project của bạn)
// ============================================================
const SUPABASE_URL = "https://YOUR_PROJECT_ID.supabase.co";
const SUPABASE_ANON_KEY = "YOUR_SUPABASE_ANON_KEY";

const isDemoMode = SUPABASE_URL.includes("YOUR_PROJECT_ID")
  || SUPABASE_ANON_KEY.includes("YOUR_SUPABASE_ANON_KEY")
  || !window.supabase;
const supabaseClient = !isDemoMode
  ? window.supabase.createClient(SUPABASE_URL, SUPABASE_ANON_KEY)
  : null;

document.addEventListener("DOMContentLoaded", () => {
  const state = {
    mode: "off", // 'auto' | 'manual' | 'off'
    relayOn: false,
    tempThreshold: 30.0,
    humiThreshold: 60.0,
    timer: {
      startHour: 6,
      startMin: 30,
      endHour: 7,
      endMin: 0,
      enabled: true
    },
    logs: []
  };

  // --- Tham chiếu DOM ---
  const dateEl = document.getElementById("current-date");
  const timeEl = document.getElementById("current-time");
  const connBadgeEl = document.getElementById("connection-badge");
  const connTextEl = document.getElementById("connection-text");
  const pumpBadgeEl = document.getElementById("pump-badge");
  const pumpTextEl = document.getElementById("pump-text");
  const treeMascotEl = document.getElementById("tree-mascot");

  const tempEl = document.getElementById("temp-value");
  const humiEl = document.getElementById("humi-value");

  const modeButtons = document.querySelectorAll(".ctrl-btn[data-mode]");
  const btnTimerOpen = document.getElementById("btn-timer");
  const timerBadgeText = document.getElementById("timer-badge-text");

  const timerModal = document.getElementById("timer-modal");
  const btnCloseModal = document.getElementById("btn-close-modal");
  const btnCancelTimer = document.getElementById("btn-cancel-timer");
  const btnSaveTimer = document.getElementById("btn-save-timer");
  const timerEnabledInput = document.getElementById("timer-enabled");

  const pad2 = (n) => String(n).padStart(2, "0");

  // ============================================================
  // 2. ĐỒNG HỒ NGÀY & GIỜ REALTIME
  // ============================================================
  function updateLocalClock() {
    const now = new Date();
    dateEl.textContent = `${pad2(now.getDate())}/${pad2(now.getMonth() + 1)}/${now.getFullYear()}`;
    timeEl.textContent = `${pad2(now.getHours())} : ${pad2(now.getMinutes())} : ${pad2(now.getSeconds())}`;
  }
  updateLocalClock();
  setInterval(updateLocalClock, 1000);

  // ============================================================
  // 3. CẬP NHẬT TRẠNG THÁI NÚT BẤM & MÁY BƠM
  // ============================================================
  function setConnectionStatus(ok, customLabel) {
    connBadgeEl.classList.toggle("connected", ok);
    connBadgeEl.classList.toggle("disconnected", !ok);
    connTextEl.textContent = customLabel || (ok ? "Đã kết nối" : "Mất kết nối");
  }

  function renderDeviceStateUI() {
    modeButtons.forEach((btn) => {
      btn.classList.toggle("active", btn.dataset.mode === state.mode);
    });

    pumpBadgeEl.classList.toggle("running", state.relayOn);
    pumpTextEl.textContent = state.relayOn ? "Bơm: Đang chạy" : "Bơm: Đang tắt";
    treeMascotEl.classList.toggle("active-breeze", state.relayOn);

    if (state.timer.enabled) {
      const sStr = `${pad2(state.timer.startHour)}:${pad2(state.timer.startMin)}`;
      const eStr = `${pad2(state.timer.endHour)}:${pad2(state.timer.endMin)}`;
      timerBadgeText.textContent = `${sStr} – ${eStr}`;
      timerBadgeText.classList.remove("hidden");
    } else {
      timerBadgeText.classList.add("hidden");
    }

    document.getElementById("limits-text").textContent =
      `Ngưỡng tự động: ≥ ${state.tempThreshold.toFixed(1)}°C · ≤ ${state.humiThreshold.toFixed(1)}%`;
  }

  // ============================================================
  // 4. VẼ BIỂU ĐỒ ĐƯỜNG MỀM VỚI DẢI MÀU NHẠT PHÍA DƯỚI
  // ============================================================
  function buildSparklineSVG(values, strokeColor, gradId) {
    if (!values || values.length === 0) return "";
    const width = 320;
    const height = 52;
    const padX = 6;
    const padY = 8;

    const minVal = Math.min(...values);
    const maxVal = Math.max(...values);
    const span = maxVal - minVal || 1;

    const points = values.map((v, idx) => {
      const x = values.length === 1
        ? width / 2
        : padX + (idx / (values.length - 1)) * (width - padX * 2);
      const y = height - padY - ((v - minVal) / span) * (height - padY * 2);
      return { x, y };
    });

    const linePts = points.map((p) => `${p.x.toFixed(1)},${p.y.toFixed(1)}`).join(" ");
    const firstX = points[0].x.toFixed(1);
    const lastX = points[points.length - 1].x.toFixed(1);
    const areaPts = `${firstX},${height} ${linePts} ${lastX},${height}`;

    const circles = points
      .map((p) => `<circle cx="${p.x.toFixed(1)}" cy="${p.y.toFixed(1)}" r="3.2" fill="#ffffff" stroke="${strokeColor}" stroke-width="2.2" />`)
      .join("");

    return `
      <svg viewBox="0 0 ${width} ${height}" preserveAspectRatio="none">
        <defs>
          <linearGradient id="${gradId}" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stop-color="${strokeColor}" stop-opacity="0.22" />
            <stop offset="100%" stop-color="${strokeColor}" stop-opacity="0.0" />
          </linearGradient>
        </defs>
        <polygon fill="url(#${gradId})" points="${areaPts}" />
        <polyline
          fill="none"
          stroke="${strokeColor}"
          stroke-width="2.3"
          stroke-linecap="round"
          stroke-linejoin="round"
          points="${linePts}"
        />
        ${circles}
      </svg>
    `;
  }

  function renderAnalyticsUI() {
    const logs = state.logs;
    if (!logs.length) return;

    const latest = logs[logs.length - 1];
    const temps = logs.map((d) => Number(d.temperature));
    const humis = logs.map((d) => Number(d.humidity));

    tempEl.textContent = Number(latest.temperature ?? 0).toFixed(1);
    humiEl.textContent = Number(latest.humidity ?? 0).toFixed(1);

    const avgT = temps.reduce((a, b) => a + b, 0) / temps.length;
    const avgH = humis.reduce((a, b) => a + b, 0) / humis.length;
    document.getElementById("avg-temp").textContent = `${avgT.toFixed(1)}°C`;
    document.getElementById("avg-humi").textContent = `${avgH.toFixed(1)}%`;
    document.getElementById("point-count").textContent = `${logs.length} điểm`;

    document.getElementById("temp-max-label").textContent = `${Math.max(...temps).toFixed(1)}°`;
    document.getElementById("temp-min-label").textContent = `${Math.min(...temps).toFixed(1)}°`;
    document.getElementById("humi-max-label").textContent = `${Math.max(...humis).toFixed(1)}%`;
    document.getElementById("humi-min-label").textContent = `${Math.min(...humis).toFixed(1)}%`;

    document.getElementById("temp-sparkline").innerHTML = buildSparklineSVG(temps, "#16a34a", "grad-temp");
    document.getElementById("humi-sparkline").innerHTML = buildSparklineSVG(humis, "#0ea5e9", "grad-humi");

    if (logs[0].created_at) {
      const firstDate = new Date(logs[0].created_at);
      document.getElementById("oldest-time-label").textContent =
        `${pad2(firstDate.getHours())}:${pad2(firstDate.getMinutes())}`;
    }
  }

  // ============================================================
  // 5. POPUP HẸN GIỜ MỖI NGÀY
  // ============================================================
  function renderTimerInputs() {
    document.getElementById("start-hour").textContent = pad2(state.timer.startHour);
    document.getElementById("start-min").textContent = pad2(state.timer.startMin);
    document.getElementById("end-hour").textContent = pad2(state.timer.endHour);
    document.getElementById("end-min").textContent = pad2(state.timer.endMin);
    timerEnabledInput.checked = state.timer.enabled;
  }

  document.querySelectorAll(".spinner-box").forEach((box) => {
    const field = box.dataset.field;
    const maxVal = field.includes("Hour") ? 23 : 59;

    const activateBox = () => {
      document.querySelectorAll(".spinner-box").forEach((b) => b.classList.remove("active"));
      box.classList.add("active");
    };

    box.querySelector(".up").addEventListener("click", () => {
      activateBox();
      state.timer[field] = state.timer[field] + 1 > maxVal ? 0 : state.timer[field] + 1;
      renderTimerInputs();
    });

    box.querySelector(".down").addEventListener("click", () => {
      activateBox();
      state.timer[field] = state.timer[field] - 1 < 0 ? maxVal : state.timer[field] - 1;
      renderTimerInputs();
    });
  });

  btnTimerOpen.addEventListener("click", () => {
    renderTimerInputs();
    timerModal.classList.remove("hidden");
  });

  const closeModal = () => timerModal.classList.add("hidden");
  btnCloseModal.addEventListener("click", closeModal);
  btnCancelTimer.addEventListener("click", closeModal);
  timerModal.addEventListener("click", (e) => {
    if (e.target === timerModal) closeModal();
  });

  // ============================================================
  // 6. GỬI LỆNH LÊN SUPABASE
  // ============================================================
  async function updateDeviceStateOnCloud(patch) {
    if (isDemoMode) {
      if (patch.mode !== undefined) {
        state.mode = patch.mode;
        state.relayOn = patch.mode === "manual";
      }
      renderDeviceStateUI();
      return;
    }

    const { error } = await supabaseClient
      .from("device_state")
      .update({ ...patch, updated_at: new Date().toISOString() })
      .eq("id", 1);

    if (error) {
      console.error("Lỗi cập nhật Supabase:", error);
      setConnectionStatus(false, "Lỗi đồng bộ");
    }
  }

  modeButtons.forEach((btn) => {
    btn.addEventListener("click", async () => {
      const selectedMode = btn.dataset.mode;
      state.mode = selectedMode;
      renderDeviceStateUI();
      await updateDeviceStateOnCloud({ mode: selectedMode });
    });
  });

  btnSaveTimer.addEventListener("click", async () => {
    state.timer.enabled = timerEnabledInput.checked;
    closeModal();
    renderDeviceStateUI();

    const startStr = `${pad2(state.timer.startHour)}:${pad2(state.timer.startMin)}`;
    const endStr = `${pad2(state.timer.endHour)}:${pad2(state.timer.endMin)}`;

    await updateDeviceStateOnCloud({
      timer_enabled: state.timer.enabled,
      start_time: startStr,
      end_time: endStr
    });
  });

  // ============================================================
  // 7. ĐỒNG BỘ DỮ LIỆU REALTIME TỪ SUPABASE
  // ============================================================
  function applyRemoteStateRow(row) {
    if (!row) return;
    state.mode = row.mode || "off";
    state.relayOn = Boolean(row.relay_on);
    if (row.temp_threshold !== undefined) state.tempThreshold = Number(row.temp_threshold);
    if (row.humi_threshold !== undefined) state.humiThreshold = Number(row.humi_threshold);
    state.timer.enabled = Boolean(row.timer_enabled);

    if (row.start_time && row.start_time.includes(":")) {
      const [sh, sm] = row.start_time.split(":").map(Number);
      state.timer.startHour = sh;
      state.timer.startMin = sm;
    }
    if (row.end_time && row.end_time.includes(":")) {
      const [eh, em] = row.end_time.split(":").map(Number);
      state.timer.endHour = eh;
      state.timer.endMin = em;
    }

    renderDeviceStateUI();
  }

  async function initCloudSync() {
    if (isDemoMode) {
      setConnectionStatus(true, "Bản xem thử");
      state.logs = [
        { temperature: 29.8, humidity: 72.8 },
        { temperature: 30.0, humidity: 73.5 },
        { temperature: 30.1, humidity: 73.0 },
        { temperature: 30.1, humidity: 72.3 },
        { temperature: 30.3, humidity: 72.3 },
        { temperature: 30.3, humidity: 71.7 },
        { temperature: 30.5, humidity: 71.2 },
        { temperature: 30.6, humidity: 70.4 },
        { temperature: 30.8, humidity: 70.1 },
        { temperature: 30.9, humidity: 69.9 }
      ];
      renderDeviceStateUI();
      renderAnalyticsUI();
      return;
    }

    const { data: stateData, error: stateErr } = await supabaseClient
      .from("device_state")
      .select("*")
      .eq("id", 1)
      .single();

    if (stateErr) {
      setConnectionStatus(false, "Mất kết nối");
    } else {
      setConnectionStatus(true, "Đã kết nối");
      applyRemoteStateRow(stateData);
    }

    const { data: logsData } = await supabaseClient
      .from("sensor_logs")
      .select("temperature, humidity, created_at")
      .order("created_at", { ascending: false })
      .limit(10);

    if (logsData && logsData.length > 0) {
      state.logs = logsData.reverse();
      renderAnalyticsUI();
    }

    supabaseClient
      .channel("nong-trai-realtime")
      .on(
        "postgres_changes",
        { event: "UPDATE", schema: "public", table: "device_state", filter: "id=eq.1" },
        (payload) => {
          setConnectionStatus(true, "Đã kết nối");
          applyRemoteStateRow(payload.new);
        }
      )
      .on(
        "postgres_changes",
        { event: "INSERT", schema: "public", table: "sensor_logs" },
        (payload) => {
          setConnectionStatus(true, "Đã kết nối");
          state.logs.push(payload.new);
          if (state.logs.length > 10) state.logs.shift();
          renderAnalyticsUI();
        }
      )
      .subscribe((status) => {
        if (status === "SUBSCRIBED") setConnectionStatus(true, "Đã kết nối");
        if (status === "CHANNEL_ERROR" || status === "TIMED_OUT") {
          setConnectionStatus(false, "Mất kết nối");
        }
      });
  }

  initCloudSync();
  if ("serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("./sw.js").catch((err) => {
      console.warn("Lỗi đăng ký Service Worker:", err);
    });
  });
}
});
