
 
    // === Авторизация ===
    loginForm.addEventListener('submit', async (e) => {
        e.preventDefault();

        const data = new URLSearchParams(new FormData(e.target));
         console.log(`Submitting login for user: ${data.get('login')}`);
        const res = await fetch('/login', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: data.toString()
        });
        console.log("Login response status:", res.status);
        if (res.ok) {
            const json = await res.json();
            sessionStorage.setItem('auth_token', json.token);
            buildNumber = json.build_number;
            buildDate = json.build_date;
    
            console.log(`Версия сборки: #${buildNumber} (${buildDate})`);

            const buildInfoText = document.getElementById("buildInfoText");
            if (buildInfoText) {
                buildInfoText.textContent = `Build #${buildNumber} • ${buildDate}`;
            }
            isLoggingOut = false;
            initWebSocket();
           // Показ успешного сообщения
           loginStatus.textContent = "Вход выполнен";
           loginStatus.style.color = "#4CAF50"; // зелёный
           loginStatus.classList.remove("hidden");
          
            setTimeout(async () => {
               const settings = await fetchSettings();
               if (settings) {
                    showMainScreen();
                }
                // Загружаем настройки сразу после входа
                const activeTab = document.querySelector(".menu-item.active")?.dataset.tab || "account";
                sideMenu.classList.remove("open");
                await loadTabSettings(activeTab);
            }, 1000);

        } else {
            const text = await res.text();
               // Показ  сообщения
           loginStatus.textContent = "Ошибка входа";
           loginStatus.style.color = "red"; // красный
           loginStatus.classList.remove("hidden");
           setTimeout(async () => {
            loginStatus.classList.add("hidden");
           }, 1000);
        
        }
    });




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




async function loadTabSettings(tab) {
    const token = sessionStorage.getItem("auth_token");
    if (!token) return;

    try {
        const endpoints = {
            "LAN": "/get_settings/network",
            "wifi": "/get_settings/wifi",
            "uart": "/get_settings/uart",
            "user": "/get_settings/user",
            "system": "/get_settings/system",
            "account": "/get_settings/user"
        };

        const url = endpoints[tab];
        if (!url) return;

        const res = await fetch(url, {
            method: "GET",
            headers: {
                "Authorization": `Bearer ${token}`
            }
        });

        if (res.status === 401) {
            sessionStorage.removeItem("auth_token");
            showTokenExpiredModal();
            return;
        }

        if (!res.ok) {
            console.warn(`Ошибка загрузки ${tab}:`, await res.text());
            return;
        }

        const data = await res.json();
        console.log(`Настройки для ${tab}:`, data);

        applySettingsToForm(tab, data);

    } catch (err) {
        console.error("Ошибка загрузки вкладки:", err);
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

                   // 🔐 Токен истёк
                if (res.status === 401) {
                    sessionStorage.removeItem("auth_token");
                    showTokenExpiredModal();
                    showLogin();
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
                        message: json.message || "Ошибка сохранения пользователя"
                    });
                    return;
                }

            // ✅ Успех
            showPopup({
                type: "success",
                title: "Сохранено",
                message: json.message || "Настройки пользователя сохранены 💾",
                timeout: 3000
            });

        

    } catch (err) {
        console.error("User save failed:", err);
        showPopup({
            type: "error",
            title: "Нет соединения",
            message: "Не удалось сохранить настройки пользователя"
        });
    }
        });



saveNetworkBtn.addEventListener("click", async (e) => {
    e.preventDefault();

    const token = sessionStorage.getItem("auth_token");
    if (!token) {
        showLogin();
        return;
    }

    const netType = document.querySelector('input[name="netType"]:checked').value;

    const payload = {
        dhcp_enabled: document.getElementById("dhcp").checked,
        ip: document.getElementById("ip").value.trim(),
        mask: document.getElementById("mask").value.trim(),
        gateway: document.getElementById("gateway").value.trim(),
        dns: document.getElementById("dns").value.trim(),

        wifi_dhcp_enabled: document.getElementById("wifi_dhcp").checked,
        wifi_ip: document.getElementById("wifi_ip").value.trim(),
        wifi_mask: document.getElementById("wifi_mask").value.trim(),
        wifi_gateway: document.getElementById("wifi_gateway").value.trim(),
        wifi_dns: document.getElementById("wifi_dns").value.trim(),

        port: parseInt(document.getElementById("port").value, 10),
        net_type: netType // ethernet | wifi (если понадобится)
    };

    console.log("Saving NETWORK:", payload);

    try {
        const res = await fetch("/save_settings/network", {
            method: "POST",
            headers: {
                "Content-Type": "application/json",
                "Authorization": "Bearer " + token
            },
            body: JSON.stringify(payload)
        });

        if (res.status === 401) {
            sessionStorage.removeItem("auth_token");
            showTokenExpiredModal();
            showLogin();
            return;
        }

        let json = {};
        try { json = await res.json(); } catch {}

        if (!res.ok) {
            showPopup({
                type: "error",
                title: "Ошибка",
                message: json.message || "Ошибка сохранения сети"
            });
            return;
        }

        showPopup({
            type: "success",
            title: "Сохранено",
            message: json.message || "Сетевые настройки сохранены 💾",
            timeout: 3000
        });

    } catch (err) {
        console.error("Network save failed:", err);
        showPopup({
            type: "error",
            title: "Нет соединения",
            message: "Не удалось сохранить сетевые настройки"
        });
    }
});


