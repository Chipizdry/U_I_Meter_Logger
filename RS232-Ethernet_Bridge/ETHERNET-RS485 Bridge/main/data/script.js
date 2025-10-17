

document.addEventListener("DOMContentLoaded", () => {
    const loginForm = document.getElementById('loginForm');
    const wifiForm = document.getElementById("wifiForm");
    const mainScreen = document.getElementById("mainScreen");
    const logoutBtn = document.getElementById("logoutBtn");

    // Проверяем, есть ли уже токен
    const token = sessionStorage.getItem('auth_token');
    if (token) {
        showMainScreen();
    } else {
        showLogin();
    }

    // === Авторизация ===
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
            showMainScreen();
        } else {
            const text = await res.text();
            alert('Ошибка входа 💩: ' + text);
        }
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

    // === Выход из аккаунта ===
    logoutBtn.addEventListener("click", () => {
        sessionStorage.removeItem("auth_token");
        alert("Вы вышли из системы 👋");
        showLogin();
    });

    // === Функции отображения ===
    function showMainScreen() {
        loginForm.classList.add("hidden");
        mainScreen.classList.remove("hidden");
    }

    function showLogin() {
        loginForm.classList.remove("hidden");
        mainScreen.classList.add("hidden");
    }
});


