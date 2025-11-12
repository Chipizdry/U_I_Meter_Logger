

document.addEventListener("DOMContentLoaded", () => {
    const loginForm = document.getElementById('loginForm');
    const wifiForm = document.getElementById("wifiForm");
    const mainScreen = document.getElementById("mainScreen");
    const logoutBtn = document.getElementById("logoutBtn");
    const ipFields = ["ip", "mask", "gateway", "dns"];
    const dhcpCheckbox = document.getElementById("dhcp");
    const eye = document.getElementById("eyeIcon");
    updateEyeIcon(eye, false); // false = закрытый глаз

    let buildNumber = null;
    let buildDate = null;

    ipFields.forEach(id => {
        const field = document.getElementById(id);
        if (field) applyIPMask(field);
    });


// === Включение / выключение DHCP ===
    function updateIPFieldsState() {
        const disabled = dhcpCheckbox.checked;
        ipFields.forEach(id => {
            const field = document.getElementById(id);
            field.disabled = disabled;
            field.style.opacity = disabled ? "0.6" : "1.0";
        });
    }
    dhcpCheckbox.addEventListener("change", updateIPFieldsState);

    // Проверяем, есть ли уже токен
    const token = sessionStorage.getItem('auth_token');
    if (token) {
        showMainScreen();
        fetchSettings();
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
            buildNumber = json.build_number;
            buildDate = json.build_date;
    
            console.log(`Версия сборки: #${buildNumber} (${buildDate})`);

            const buildInfoText = document.getElementById("buildInfoText");
            if (buildInfoText) {
                buildInfoText.textContent = `Build #${buildNumber} • ${buildDate}`;
            }
    
            alert('Авторизация успешна ✅');
            fetchSettings();
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
        item.addEventListener("click", async () => {
            // Переключаем вкладки UI
            menuItems.forEach(i => i.classList.remove("active"));
            tabContents.forEach(t => t.classList.remove("active"));

            item.classList.add("active");
            const tabId = item.dataset.tab;
            document.getElementById(tabId).classList.add("active");

            // Закрываем меню на мобилке
            sideMenu.classList.remove("open");

            // Загружаем настройки для конкретной вкладки
            await loadTabSettings(tabId);
        });
    });


    async function loadTabSettings(tab) {
        const token = sessionStorage.getItem("auth_token");
        if (!token) return;
    
        try {
            const endpoints = {
                "LAN": "/get_settings/network",
                "wifi": "/get_settings/network",
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


    function applySettingsToForm(tab, data) {
        switch (tab) {
            case "LAN":
         /*       document.getElementById("ip").value = data.ip || "";
                document.getElementById("mask").value = data.mask || "";
                document.getElementById("gateway").value = data.gateway || "";
                document.getElementById("dns").value = data.dns || "";
                document.getElementById("dhcp").checked = !!data.dhcp;
                document.getElementById("port").value = data.port || 80; */
                break;
    
            case "wifi":
                document.querySelector("[name='ssid']").value = data.ssid || "";
                document.querySelector("[name='password']").value = data.password || "";
                document.querySelector("[name='mode']").value = data.mode || "STA";
                break;
    
            case "uart":
                // Пример подстановки, если такие поля будут
                document.querySelector("#uart input[type=number]").value = data.baud || 9600;
                break;
    
            case "user":
               
                document.querySelector("[name='user-login']").value = data.user.login || "";
                break;
    
            case "account":
               
                document.querySelector("[name='account-login']").value = data.user.account_login || "";
                break;
    
            case "system":
                // Тут заполняй по аналогии, если есть поля
                break;
        }
    }
    


 // Функция: форматирует IP (добавляет нули и точки)
 function formatIP(value) {
    // Убираем всё, кроме цифр
    let digits = value.replace(/\D/g, '');
    // Разбиваем по 3 цифры
    let parts = [];
    for (let i = 0; i < digits.length; i += 3) {
        parts.push(digits.substr(i, 3));
    }
    // Если неполный блок — дополним нулями
    parts = parts.map(p => p.padStart(3, '0'));
    return parts.join('.').substring(0, 15);
}

  // === Проверка корректности IP ===
function validateIP(ip) {
    const parts = ip.split(".");
    if (parts.length !== 4) return false;
    return parts.every(p => {
        const num = parseInt(p, 10);
        return !isNaN(num) && num >= 0 && num <= 255;
    });
}



 
 // === Форматирование и маска ввода ===
 function applyIPMask(input) {
     input.addEventListener("input", (e) => {
         let value = e.target.value.replace(/[^\d]/g, "");
         let parts = [];
 
         // Разбиваем по 3 цифры (но не добавляем ведущие нули)
         for (let i = 0; i < value.length && parts.length < 4; i += 3) {
             parts.push(value.substring(i, i + 3));
         }
 
         // Соединяем с точками
         e.target.value = parts.join(".");
 
         // Автопереход курсора, когда введено 3 цифры и нет точки
         if (e.inputType === "insertText" && value.length < 12 && value.length % 3 === 0 && !e.target.value.endsWith(".")) {
             e.target.value += ".";
         }
     });
 
     // При фокусе показываем шаблон, если пусто
     input.addEventListener("focus", (e) => {
         if (e.target.value.trim() === "") e.target.placeholder = "___.___.___.___";
     });
 
     // При потере фокуса — завершаем IP и проверяем
     input.addEventListener("blur", (e) => {
         let parts = e.target.value.split(".").filter(Boolean);
 
         // Дополняем до 4 октетов
         while (parts.length < 4) parts.push("0");
 
         // Убираем лишние ведущие нули
         parts = parts.map(p => String(parseInt(p || "0", 10)));
 
         e.target.value = parts.join(".");
 
         // Проверка корректности
         if (!validateIP(e.target.value)) {
             e.target.style.border = "2px solid red";
             e.target.title = "Некорректный IP адрес";
         } else {
             e.target.style.border = "";
             e.target.title = "";
         }
     });
 }
 

    // === Обработчик сохранения ===
    document.getElementById("lanForm").addEventListener("submit", (e) => {
        e.preventDefault();
        const formData = new FormData(e.target);
        const params = new URLSearchParams(formData);

        // Проверяем все IP
        const invalid = ipFields.find(id => !validateIP(document.getElementById(id).value));
        if (invalid) {
            alert(`Поле "${invalid}" заполнено неверно 💩`);
            document.getElementById(invalid).focus();
            return;
        }

        alert("Настройки сети сохранены ✅");
        console.log("LAN settings:", Object.fromEntries(formData));
        // TODO: fetch('/save_network', {...})
    });


// === Функция получения настроек с сервера ===
    async function fetchSettings() {
        const token = sessionStorage.getItem("auth_token");
        if (!token) return;

        try {
            const res = await fetch("/get_settings", {
                method: "GET",
                headers: {
                    "Authorization": `Bearer ${token}`
                }
            });

            if (res.status === 401) {
               // alert("Сессия истекла, пожалуйста, авторизуйтесь снова 💩");
                sessionStorage.removeItem("auth_token");
                showTokenExpiredModal();
                showLogin();
                return;
            }

            if (!res.ok) {
                const text = await res.text();
                alert(`Ошибка получения настроек: ${text}`);
                return;
            }

            const data = await res.json();
            console.log("Полученные настройки:", data);

            // === Заполняем форму LAN ===
            if (data.network) {
                const net = data.network;
                document.getElementById("ip").value = net.ip || "";
                document.getElementById("mask").value = net.mask || "";
                document.getElementById("gateway").value = net.gateway || "";
                document.getElementById("dns").value = net.dns || "";
                document.getElementById("dhcp").checked = !!net.dhcp_enabled;
                document.getElementById("port").value = net.port;
              //  document.getElementById("ssid").value = net.ssid || "";
              //  document.getElementById("mode").value = net.mode || "";

                // Обновляем состояние полей IP в зависимости от DHCP
                updateIPFieldsState();
            }

       
        } catch (err) {
            alert("Ошибка соединения при получении настроек: " + err);
        }
    }




});


