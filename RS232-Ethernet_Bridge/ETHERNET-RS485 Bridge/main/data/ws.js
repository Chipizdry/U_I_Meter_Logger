


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
        console.error("No auth token");
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
    if (msg.type === "update") {
        if (msg.payload.status) updateStatus(msg.payload.status);
    }

    // любые другие типы:
    console.log("WS msg:", msg);
}

function updateStatus(text) {
    const el = document.getElementById("ws-status");
    if (el) el.innerText = text;
}

window.addEventListener("load", initWebSocket);

