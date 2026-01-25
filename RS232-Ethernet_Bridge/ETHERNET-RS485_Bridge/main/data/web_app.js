


   // === Выход из аккаунта ===
logoutBtn.addEventListener("click", async () => {
    console.log("Logout...");
    isLoggingOut = true;
     shutdownWebSocket("logout");
    const token = sessionStorage.getItem("auth_token");

    // 1) Уведомляем сервер (если есть токен)
    if (token) {
        try {
            await fetch("/logout", {
                method: "POST",
                headers: {
                    "Authorization": "Bearer " + token
                }
            });
        } catch (e) {
            console.warn("Logout request failed:", e);
        }
    }

    // 2) Удаляем токен
    sessionStorage.removeItem("auth_token");

    // 3) Останавливаем heartbeat
    if (typeof stopHeartbeat === "function") {
        stopHeartbeat();
    }

    // 4) Отменяем реконнект
    if (window.reconnectTimer) {
        clearTimeout(window.reconnectTimer);
        window.reconnectTimer = null;
    }

    // 5) Отключаем WS
    if (window.ws) {
        console.log("Closing WebSocket (logout)");
        ws.onopen = null;
        ws.onmessage = null;
        ws.onerror = null;
        ws.onclose = null; // 💥 обязательно
        ws.close(1000, "Logout");
        ws = null;
    }

    // 6) Возврат к экрану логина
    //alert("Вы вышли из системы 👋");
     updateStatus("Disconnected");
    showLogin();
    loginStatus.classList.add("hidden");
});




async function FactoryDefaults() {
    const token = sessionStorage.getItem("auth_token");

    if (!token) {
        showLogin();
        return;
    }

    const confirmed = confirm(
        "⚠️ СБРОС К ЗАВОДСКИМ НАСТРОЙКАМ\n\n" +
        "Будут удалены:\n" +
        "• Wi-Fi настройки\n" +
        "• Аккаунт и токены\n" +
        "• Все пользовательские параметры\n\n" +
        "Устройство перезагрузится.\n\n" +
        "Продолжить?"
    );

    if (!confirmed) return;

    console.warn("Factory reset requested…");

    try {
        const response = await fetch("/factory_reset", {
            method: "POST",
            headers: {
                "Authorization": "Bearer " + token
            }
        });

        if (response.status === 401) {
            sessionStorage.removeItem("auth_token");
            showTokenExpiredModal();
            return;
        }

        if (!response.ok) {
            const text = await response.text();
            alert("Ошибка сброса: " + text);
            return;
        }

        const json = await response.json();
        console.log("Factory reset:", json);

        // 🔥 Локальная очистка
        sessionStorage.removeItem("auth_token");

        // 🔥 Останавливаем всё
        if (typeof stopHeartbeat === "function") stopHeartbeat();

        if (window.reconnectTimer) {
            clearTimeout(window.reconnectTimer);
            window.reconnectTimer = null;
        }

        if (window.ws) {
            ws.onclose = null;
            ws.close();
            window.ws = null;
        }

        // 🔥 UX
        showFactoryResetInProgress();

        // Через ~5 сек устройство будет уже в AP
        setTimeout(() => {
            window.location.href = "http://192.168.4.1/";
        }, 6000);

    } catch (err) {
        console.error("Factory reset failed:", err);
       // alert("Ошибка соединения: " + err);
    }
}



async function rebootDevice() {
    const token = sessionStorage.getItem("auth_token");

    try {
        const res = await fetch("/reboot", {
            method: "POST",
            headers: { "Authorization": "Bearer " + token }
        });

        if (res.status === 401) {
            sessionStorage.removeItem("auth_token");
            showTokenExpiredModal();
            return;
        }

        const json = await res.json();

        showPopup({
            type: "info",
            title: "Перезапуск...",
            message: json.message,
            timeout: 4000
        });

    } catch (err) {
        showPopup({
            type: "error",
            title: "Ошибка",
            message: "Нет соединения с устройством"
        });
    }
}

async function saveUARTSettings() {
    const token = sessionStorage.getItem("auth_token");
    if (!token) {
        showLogin();
        return;
    }

    const mode = parseInt(
        document.querySelector('input[name="uart-mode"]:checked')?.value || 0
    );

    const payload = {
        mode,
        baud: parseInt(document.getElementById('uart-baud').value),
        parity: parseInt(document.getElementById('uart-parity').value),
        stop_bits: parseInt(document.getElementById('uart-stop-bits').value),
        data_bits: parseInt(document.getElementById('uart-data-bits').value)
    };

    try {
        const res = await fetch('/save_settings/uart', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': 'Bearer ' + token
            },
            body: JSON.stringify(payload)
        });

        // 🔐 Сессия истекла
        if (res.status === 401) {
            sessionStorage.removeItem("auth_token");
            showTokenExpiredModal();
            return;
        }

        let json = {};
        try {
            json = await res.json();
        } catch (_) {}

        if (!res.ok) {
            showPopup({
                type: "error",
                title: "Ошибка",
                message: json.message || "Ошибка сохранения UART настроек"
            });
            return;
        }

        // ✅ Успех
        showPopup({
            type: "success",
            title: "Сохранено",
            message: json.message || "UART настройки сохранены",
            timeout: 3000
        });

    } catch (err) {
        console.error("UART save failed:", err);
        showPopup({
            type: "error",
            title: "Нет соединения",
            message: "Не удалось сохранить UART настройки"
        });
    }
}



async function saveWiFiSettings() {
    const token = sessionStorage.getItem("auth_token");
    if (!token) {
        showLogin();
        return;
    }

    const wifiForm = document.getElementById("wifiForm");

    const payload = {
        mode: parseInt(document.getElementById("wifiMode").value),

        sta_ssid: wifiForm.querySelector("[name='sta-ssid']").value,
        sta_password: wifiForm.querySelector("[name='sta-password']").value,

        ap_ssid: wifiForm.querySelector("[name='ap-ssid']").value,
        ap_password: wifiForm.querySelector("[name='ap-password']").value,
        ap_channel: parseInt(
            wifiForm.querySelector("[name='ap-channel']").value
        ),
    };

    try {
        const res = await fetch('/save_settings/wifi', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': 'Bearer ' + token
            },
            body: JSON.stringify(payload)
        });

        // 🔐 Сессия истекла
        if (res.status === 401) {
            sessionStorage.removeItem("auth_token");
            showTokenExpiredModal();
            return;
        }

        let json = {};
        try {
            json = await res.json();
        } catch (_) {
            // backend может вернуть не JSON
        }

        if (!res.ok) {
            showPopup({
                type: "error",
                title: "Ошибка",
                message: json.message || "Ошибка сохранения Wi-Fi настроек"
            });
            return;
        }

        // ✅ Успех
        showPopup({
            type: "success",
            title: "Сохранено",
            message: json.message || "Wi-Fi настройки сохранены",
            timeout: 3000
        });

    } catch (err) {
        console.error("WiFi save failed:", err);
        showPopup({
            type: "error",
            title: "Нет соединения",
            message: "Не удалось сохранить Wi-Fi настройки"
        });
    }
}


/*
   // === Сохранение Wi-Fi настроек ===
wifiForm.addEventListener("submit", async (e) => {
    e.preventDefault();

    const formData = new FormData(wifiForm);
    const params = new URLSearchParams(formData);
    const token = sessionStorage.getItem('auth_token');

    console.log("Отправляем токен:", token);

    try {
        const response = await fetch("/save_settings", {
            method: "POST",
            headers: {
                "Content-Type": "application/x-www-form-urlencoded",
                "Authorization": `Bearer ${token}`
            },
            body: params.toString()
        });

        // Если сервер вернул 401 — токен недействителен
        if (response.status === 401) {
           // alert("Сессия истекла, пожалуйста, авторизуйтесь снова 💩");
            sessionStorage.removeItem("auth_token");
            showTokenExpiredModal();
           // showLogin();
            return;
        }

        const text = await response.text();

        if (response.ok) {
            alert(text);
        } else {
            alert(`Ошибка: ${text}`);
        }
    } catch (err) {
        alert(`Ошибка соединения: ${err}`);
    }
});

*/
    // === Сохранение настроек аккаунта ===
saveAccountBtn.addEventListener('click', async () => {
    const token = sessionStorage.getItem("auth_token");
    if (!token) {
        showLogin();
        return;
    }

    const formData = new FormData(accountForm);
    const data = new URLSearchParams(formData);

    try {
        const res = await fetch('/save_settings/account', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/x-www-form-urlencoded',
                'Authorization': 'Bearer ' + token
            },
            body: data.toString()
        });

        // 🔐 Сессия истекла
        if (res.status === 401) {
            sessionStorage.removeItem("auth_token");
            showTokenExpiredModal();
            return;
        }

        let json = {};
        try {
            json = await res.json();
        } catch (_) {
            // backend может вернуть невалидный JSON
        }

        if (!res.ok) {
            showPopup({
                type: "error",
                title: "Ошибка",
                message: json.message || "Ошибка сохранения настроек аккаунта"
            });
            return;
        }

        // ✅ Успех
        showPopup({
            type: "success",
            title: "Сохранено",
            message: json.message || "Настройки аккаунта сохранены",
            timeout: 3000
        });

    } catch (err) {
        console.error("Account save failed:", err);
        showPopup({
            type: "error",
            title: "Нет соединения",
            message: "Не удалось сохранить настройки аккаунта"
        });
    }
});





    // === Сохранение настроек пользователя ===

        userForm.addEventListener("submit", async (e) => {
            e.preventDefault();

            const token = sessionStorage.getItem("auth_token");
            if (!token) {
                showLogin();
                return;
            }

            const login = userForm.querySelector('[name="user-login"]').value.trim();
            const pass = userForm.querySelector('[name="user-password"]').value.trim();
            const confirm = userForm.querySelector('[name="user-confirm-password"]').value.trim();

            // === Проверка пароля ===
            if (pass !== confirm) {
                alert("Пароли не совпадают!");
                return;
            }

            if (pass.length < 6) {
                alert("Пароль должен содержать минимум 6 символа");
                return;
            }

            // Готовим данные
            const params = new URLSearchParams();
            params.append("user-login", login);
            params.append("user-password", pass);

            console.log("Сохраняем USER:", { login, pass });

            try {
                const res = await fetch("/save_settings/user", {
                    method: "POST",
                    headers: {
                        'Content-Type': 'application/x-www-form-urlencoded',
                        'Authorization': `Bearer ${token}`
                    },
                    body: params.toString()
                });

                if (res.status === 401) {
                    sessionStorage.removeItem("auth_token");
                    showTokenExpiredModal();
                    showLogin();
                    return;
                }

                const text = await res.text();

                if (!res.ok) {
                    console.error("Ошибка сохранения пользователя:", text);
                    alert("Ошибка: " + text);
                    return;
                }

                console.log("USER settings saved:", text);

                // Статус в интерфейсе
                const wsStatus = document.getElementById("ws-status");
                if (wsStatus) {
                    wsStatus.textContent = "Пользователь обновлён ✔️";
                    wsStatus.style.color = "#4CAF50";
                    setTimeout(() => wsStatus.textContent = "", 2000);
                }

                alert("Настройки пользователя сохранены");

            } catch (err) {
                console.error("Ошибка при сохранении пользователя:", err);
                alert("Ошибка: " + err);
            }
        });





        async function uploadLittleFS() {
           
            const fsFile = document.getElementById("firmwareFile").files[0];
            if (!fsFile) {
                alert("Select filesystem image!");
                return;
            }

            const otaSession = sessionStorage.getItem("ota_session");
            if (!otaSession) {
                alert("OTA session expired");
                return;
            }
            console.log("OTA session:", otaSession);
            const res = await fetch("/update_fs", {
                method: "POST",
                headers: {
                    "X-OTA-Session": otaSession
                },
                body: fsFile
            });

            if (!res.ok) {
                alert("Filesystem upload failed");
                return;
            }

            sessionStorage.removeItem("ota_session");
            alert("🎉 Update complete!");
        }



        async function waitDeviceOnline(
            timeoutMs = 90000,
            intervalMs = 1000
        ) {
            const start = Date.now();

            while (Date.now() - start < timeoutMs) {
                try {
                    const res = await fetch("/ping", {
                        method: "GET",
                        cache: "no-store"
                    });

                    if (res.ok) {
                        console.log("✅ Device is online");
                        return;
                    }
                } catch (e) {
                    console.log(" Device is offline");
                }

                await new Promise(r => setTimeout(r, intervalMs));
            }

            throw new Error("Device did not come online in time");
        }



