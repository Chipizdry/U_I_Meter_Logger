


let ws = null;
let reconnectTimer = null;
let heartbeatTimer = null;
let lastPongTime = 0;

const HEARTBEAT_INTERVAL = 10000; // 15 сек
const HEARTBEAT_TIMEOUT  = 30000; // 30 сек без pong → разрыв
const RECONNECT_DELAY    = 5000;  // 5 сек

function initWebSocket() {
    const token = sessionStorage.getItem("auth_token");
    if (!token) {
        console.log("No auth token");
        return;
    }

    // 🔥 Защита: не создавать 2 сокета
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
        console.warn("WS already active");
        return;
    }

    ws = new WebSocket(`ws://${location.host}/ws`);

    ws.onopen = () => {
        console.log("WS connected");
        updateStatus("Connected");

        // Отправляем авторизацию
        ws.send(JSON.stringify({ type: "auth", token }));

        // Ставим отметку времени pong-а
        lastPongTime = Date.now();

        startHeartbeat();
    };

    ws.onmessage = (event) => {
        const data = event.data;
    
        // Игнорируем чистые ping/pong
        if (data === "ping" || data === "pong") {
            console.log("WS heartbeat:", data);
            if (data === "ping") ws.send("pong"); // отвечаем серверу
            if (data === "pong") lastPongTime = Date.now(); // обновляем отметку времени
            return;
        }
    
        try {
            const msg = JSON.parse(data);
            handleWsMessage(msg);
            console.log("WS msg:", msg);
        } catch (err) {
            console.warn("Invalid WS message:", data);
        }
    };

    ws.onclose = () => {
        console.log("WS disconnected → reconnect in 5s");
        updateStatus("Disconnected");
        stopHeartbeat();
        ws = null;

        scheduleReconnect();
    };

    ws.onerror = (err) => {
        console.error("WS error:", err);
    };
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
    if (reconnectTimer) return;

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

window.addEventListener("load", initWebSocket);

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

