



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
