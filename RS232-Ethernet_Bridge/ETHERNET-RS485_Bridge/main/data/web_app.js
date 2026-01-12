


   // === Выход из аккаунта ===
logoutBtn.addEventListener("click", async () => {
    console.log("Logout...");

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
        console.log("Closing WebSocket on logout");
        ws.onclose = null; // 🔥 отключаем авто-реконнект
        ws.close();
        window.ws = null;
    }

    // 6) Возврат к экрану логина
    //alert("Вы вышли из системы 👋");
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


    // === Сохранение настроек аккаунта ===
    saveAccountBtn.addEventListener('click', async () => {
        const token = sessionStorage.getItem("auth_token");
        if (!token) {
            showLogin();
            return;
        }

        const formData = new FormData(accountForm);
        const data = new URLSearchParams(formData);

        console.log("Сохраняем настройки аккаунта:", {
            login: data.get('account-login'),
            password: data.get('account-password'),
            node_name: data.get('node-name'),
        });

        try {
            const res = await fetch('/save_settings/account', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded',
                    'Authorization': `Bearer ${token}`
                },
                body: data.toString()
            });

            if (res.status === 401) {
                sessionStorage.removeItem("auth_token");
                showTokenExpiredModal();
                showLogin();
                return;
            }

            if (!res.ok) {
                const text = await res.text();
                console.error("Ошибка сохранения аккаунта:", text);
                alert("Ошибка сохранения аккаунта: " + text);
                return;
            }

            const text = await res.text();
            console.log("Сохранено успешно:", text);

            if (wsStatus) {
                wsStatus.textContent = "Настройки аккаунта сохранены ✔️";
                wsStatus.style.color = "#4CAF50";
                setTimeout(() => wsStatus.textContent = "", 2000);
            }

        } catch (err) {
            console.error("Ошибка при сохранении аккаунта:", err);
            alert("Ошибка при сохранении аккаунта: " + err);
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