


let ws = null;
let reconnectTimer = null;
let heartbeatTimer = null;
let lastPongTime = 0;
let isLoggingOut = false;
const HEARTBEAT_INTERVAL = 10000; // 10 сек
const HEARTBEAT_TIMEOUT  = 30000; // 30 сек без pong → разрыв
const RECONNECT_DELAY    = 5000;  // 5 сек

function initWebSocket() {
    if (ws) {
        console.warn(`[${new Date().toISOString()}] WS already exists → skipping init`);
        return;
    }

    const token = sessionStorage.getItem("auth_token");
    if (!token) {
        console.warn(`[${new Date().toISOString()}] No auth token → cannot init WS`);
        return;
    }

    console.log(`[${new Date().toISOString()}] 🚀 Creating WS...`);

    ws = new WebSocket(`ws://${location.host}/ws`);

    ws.onopen = () => {
        console.log(`[${new Date().toISOString()}] WS connected, sending auth`);
        ws.send(JSON.stringify({ type: "auth", token }));
        lastPongTime = Date.now();
        startHeartbeat();
    };

    ws.onmessage = (event) => {
        console.log(`[${new Date().toISOString()}] WS message received:`, event.data);
        onWsMessage(event);
    };

    ws.onclose = (ev) => {
        console.log(`[${new Date().toISOString()}] WS closed, code=${ev.code}, reason=${ev.reason}, readyState=${ws.readyState}`);
        stopHeartbeat();
        ws = null;
        if (!isLoggingOut) scheduleReconnect();
    };

    ws.onerror = (err) => {
        console.error(`[${new Date().toISOString()}] WS error, readyState=${ws.readyState}`, err);
    };
}

function sendWsCommand(type, payload = {}) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        const msg = { type, payload };
        console.log(`[${new Date().toISOString()}] WS send:`, msg);
        ws.send(JSON.stringify(msg));
    } else {
        console.warn(`[${new Date().toISOString()}] WS not connected → cannot send`, type, payload);
    }
}


function startHeartbeat() {
    stopHeartbeat();

    heartbeatTimer = setInterval(() => {
        if (!ws || ws.readyState !== WebSocket.OPEN) return;

        // Пинг
        ws.send("ping");

        // Проверяем отсутствие pong
        if (Date.now() - lastPongTime > HEARTBEAT_TIMEOUT) {
            console.warn("WS heartbeat timeout → closing socket");
            ws.close();
        }
    }, HEARTBEAT_INTERVAL);
}

function stopHeartbeat() {
    if (heartbeatTimer) {
        clearInterval(heartbeatTimer);
        heartbeatTimer = null;
    }
}


function scheduleReconnect() {
    if (reconnectTimer || isLoggingOut) return;

    reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        initWebSocket();
    }, RECONNECT_DELAY);
}



function sendWsCommand(type, payload = {}) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type, payload }));
    } else {
        console.warn("WS not connected");
    }
}

function handleWsMessage(msg) {
    const cloudEl = document.getElementById("ws-status");

    // === Статусы соединения с облаком ===
    if (msg.cloud_status) {
        if (msg.cloud_status === "connected") {
            cloudEl.textContent = "☁️ Подключено к облаку";
            cloudEl.style.color = "green";
        } else if (msg.cloud_status === "disconnected") {
            cloudEl.textContent = "⚠️ Отключено от облака";
            cloudEl.style.color = "red";
        } else if (msg.cloud_status === "error") {
            cloudEl.textContent = "❌ Ошибка облака";
            cloudEl.style.color = "red";
        } else {
            cloudEl.textContent = msg.cloud_status;
            cloudEl.style.color = "blue";
        }
    }

    // === Другие типы сообщений ===
    if (msg.type === "update") {
        if (msg.payload?.status) updateStatus(msg.payload.status);
    }

   if (msg.type === "network" && msg.network) {

        function updateNetStatus(elementId, label, state) {
        const el = document.getElementById(elementId);
        if (!el) return;
        const allowedStates = ["up", "down", "connecting"];
        const status = allowedStates.includes(state) ? state : "unknown";
        el.classList.remove("up", "down", "connecting", "unknown");
        // ✅ добавляем новый класс
        el.classList.add("net-status", status);

        // текст
        el.textContent = `${label}: ${status.toUpperCase()}`;
    }

    updateNetStatus(
        "ethernet-status",
        "Ethernet",
        msg.network?.ethernet?.state
    );

    updateNetStatus(
        "wifi-status",
        "Wi-Fi",
        msg.network?.wifi_sta?.state
    );
}


    // === Wi-Fi scan started ===
    if (msg.type === "wifi_scan") {
        if (msg.status === "started") {
            const scanDiv = document.getElementById("wifiScanList");
            if (scanDiv) scanDiv.textContent = "🔄 Сканирование Wi-Fi...";
        }
    }

    // === Wi-Fi scan results ===
    if (msg.type === "wifi_scan_result" && Array.isArray(msg.aps)) {
        const scanDiv = document.getElementById("wifiScanList");
        if (!scanDiv) return;
        scanDiv.innerHTML = ""; // очищаем старый список

        if (msg.aps.length === 0) {
            scanDiv.textContent = "Сети не найдены";
            return;
        }

        const ul = document.createElement("ul");
        msg.aps.forEach(ap => {
            const li = document.createElement("li");
            li.textContent = `${ap.ssid} (RSSI: ${ap.rssi} dBm, Channel: ${ap.channel}, Auth: ${ap.authmode})`;

            // Клик по сети заполняет поле STA SSID
            li.onclick = () => {
                const ssidInput = document.querySelector("input[name='sta-ssid']");
                if (ssidInput) ssidInput.value = ap.ssid;
            };

            ul.appendChild(li);
        });

        scanDiv.appendChild(ul);
    }
    // === 🔍 Диагностика ===
    if (msg.diag) {
        addDiagRow(msg.diag);   
        return;                // дальше не идём
    }
}



 function wsScanWiFi() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        console.warn("WS not connected");
        return;
    }

    ws.send(JSON.stringify({
        action: "wifi_scan"
    }));

    console.log("WS → {action: wifi_scan}");
}





function updateStatus(text) {
    const el = document.getElementById("ws-status");
    if (el) el.innerText = text;
}


function diagnosticsOn() {
    if (diagnosticsActive || !ws || ws.readyState !== WebSocket.OPEN) return;

    diagnosticsActive = true;
    ws.send(JSON.stringify({ action: "diagnostics_on" }));
    console.log("🔍 Diagnostics ON");
}

function diagnosticsOff() {
    if (!diagnosticsActive || !ws || ws.readyState !== WebSocket.OPEN) return;

    diagnosticsActive = false;
    ws.send(JSON.stringify({ action: "diagnostics_off" }));
    console.log("🔍 Diagnostics OFF");
}




function shutdownWebSocket(reason = "manual") {
    console.log("🔌 WS shutdown:", reason);
    isLoggingOut = true;
    stopHeartbeat();
    if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
    }
    if (ws) {
        ws.onclose = null;
        ws.onerror = null;
        if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
            ws.close(1000, reason);
        }
        ws = null;
    }
}




function onWsMessage(event) {
    const data = event.data;

    // heartbeat
    if (data === "ping" || data === "pong") {
        console.log("WS heartbeat:", data);
        if (data === "ping") ws.send("pong");
        if (data === "pong") lastPongTime = Date.now();
        return;
    }

    try {
        const msg = JSON.parse(data);
        handleWsMessage(msg);
        console.log("WS msg:", msg);
    } catch (err) {
        console.warn("Invalid WS message:", data);
    }
}



