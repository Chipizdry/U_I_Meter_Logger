

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

        // Если сервер вернул 401 — токен недействителен
        if (response.status === 401) {
            alert("Сессия истекла, пожалуйста, авторизуйтесь снова 💩");
            sessionStorage.removeItem("auth_token");
            showLogin();
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



// === Гамбургер меню ===
const menuButton = document.getElementById("menuButton");
const sideMenu = document.getElementById("sideMenu");
menuButton.addEventListener("click", () => {
    sideMenu.classList.toggle("open");
});

// === Переключение вкладок ===
const menuItems = document.querySelectorAll(".menu-item[data-tab]");
const tabContents = document.querySelectorAll(".tab-content");

menuItems.forEach(item => {
    item.addEventListener("click", () => {
        // Активируем выбранную вкладку
        menuItems.forEach(i => i.classList.remove("active"));
        tabContents.forEach(t => t.classList.remove("active"));

        item.classList.add("active");
        document.getElementById(item.dataset.tab).classList.add("active");

        // Автоматически закрываем меню на мобильных
        sideMenu.classList.remove("open");
    });

    
});





});


