
document.addEventListener("DOMContentLoaded", () => {

    // === Обработчик логина ===
    const loginForm = document.getElementById('loginForm');
    if (loginForm) {
        loginForm.addEventListener('submit', async (e) => {
            e.preventDefault();

            const data = new URLSearchParams(new FormData(e.target));
            const res = await fetch('/login', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: data.toString()
            });

            if (res.ok) {
                const json = await res.json();
                sessionStorage.setItem('auth_token', json.token);
                alert('Авторизация успешна ✅');
                console.log('Токен сохранён:', json.token);
            } else {
                const text = await res.text();
                alert('Ошибка входа 💩: ' + text);
            }
        });
    }

    // === Обработчик Wi-Fi настроек ===
    const wifiForm = document.getElementById("wifiForm");
    if (wifiForm) {
        wifiForm.addEventListener("submit", async (e) => {
            e.preventDefault();

            const formData = new FormData(wifiForm);
            const params = new URLSearchParams();
            for (const [key, value] of formData.entries()) {
                params.append(key, value);
            }

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

                if (response.ok) {
                    const text = await response.text();
                    alert(text);
                } else {
                    const text = await response.text();
                    alert(`Ошибка: ${text}`);
                }
            } catch (err) {
                alert(`Ошибка соединения: ${err}`);
            }
        });
    }
});