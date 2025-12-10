


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
            password: data.get('account-password')
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
