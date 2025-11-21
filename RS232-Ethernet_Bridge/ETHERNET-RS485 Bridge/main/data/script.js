

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
        initWebSocket();  
        const activeTab = document.querySelector(".menu-item.active")?.dataset.tab || "account";
        loadTabSettings(activeTab);
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
            initWebSocket();
           // Показ успешного сообщения
           loginStatus.textContent = "Вход выполнен";
           loginStatus.style.color = "#4CAF50"; // зелёный
           loginStatus.classList.remove("hidden");
           fetchSettings();
            setTimeout(async () => {
              
                showMainScreen();
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

                // Обновляем состояние полей IP в зависимости от DHCP
                updateIPFieldsState();
            }

       
        } catch (err) {
            alert("Ошибка соединения при получении настроек: " + err);
        }
    }





});



async function saveUARTSettings() {
   
    const token = sessionStorage.getItem("auth_token");
    const payload = {
        baud: parseInt(document.getElementById('uart-baud').value),
        parity: parseInt(document.getElementById('uart-parity').value),
        stop_bits: parseInt(document.getElementById('uart-stop-bits').value),
        data_bits: parseInt(document.getElementById('uart-data-bits').value)
    };
    const res = await fetch('/save_settings/uart', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            'Authorization': 'Bearer ' + token
        },
        body: JSON.stringify(payload)
    });
    if (res.ok) {
        alert('UART настройки сохранены ✅');
    } else {
        alert('Ошибка при сохранении UART настроек ❌');
    }
}


let testAccountActive = false; // флаг тестового режима


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
        case "LAN": {
            const net = data.network || {};
            document.getElementById("ip").value = net.ip || "";
            document.getElementById("mask").value = net.mask || "";
            document.getElementById("gateway").value = net.gateway || "";
            document.getElementById("dns").value = net.dns || "";
            document.getElementById("dhcp").checked = !!net.dhcp_enabled;
            document.getElementById("port").value = net.port || 80;
            break;
        }

        case "wifi": {
            const wifi = data.wifi || {};
            document.querySelector("[name='ssid']").value = wifi.ssid || "";
            document.querySelector("[name='password']").value = wifi.password || "";
            document.querySelector("[name='mode']").value = wifi.mode || "STA";
            break;
        }

        case "uart": {
            
            const uart = data.uart || {};
              // конвертация true/false → "0"/"1"
            const rawMode = uart.rs485_mode;
            const mode = rawMode ? "1" : "0";
            document.querySelector("#uart input[type=number]").value = uart.baud || 9600;
            document.querySelector("#uart-data-bits").value = uart.data_bits || 8;
            document.querySelector("#uart-stop-bits").value = uart.stop_bits || 1;
            document.querySelector("#uart-parity").value = uart.parity || 0;
            const rb = document.querySelector(`input[name='uart-mode'][value='${mode}']`);
            if (rb) rb.checked = true;
            break;
        }

        case "user": {
            const user = data.user || {};
            document.querySelector("[name='user-login']").value = user.login || "";
            break;
        }

        case "account": {
            const account = data.user || {};
            const wsDiv = document.getElementById('ws-status');
            const color = account.connected ? 'green' : 'red';
            const icon = account.connected ? '✅' : '❌';
            document.querySelector("[name='account-login']").value = account.account_login || "";
            wsDiv.innerHTML = `COR-ID: <span style="color:${color}">${icon} ${account.status}</span> `;
            break;
        }

        case "system": {
            const system = data.system || {};
            buildNumber = system.build_number;
            buildDate = system.build_date;
    
            console.log(`Версия сборки: #${buildNumber} (${buildDate})`);

            const buildInfoText = document.getElementById("buildInfoText");
            if (buildInfoText) {
                buildInfoText.textContent = `Build #${buildNumber} • ${buildDate}`;
            }
            break;
        }

        default:
            console.warn(`Неизвестная вкладка: ${tab}`);
    }
}


const btn = document.getElementById("testAccountBtn"); // ← вот сюда

btn.addEventListener("click", () => {
    const loginInput = document.querySelector("input[name='account-login']");
    const passwordInput = document.querySelector("input[name='account-password']");
    const statusDiv = document.getElementById("ws-status");

    if (!testAccountActive) {
        // Включаем тестовый режим
        const login = loginInput.value.trim();
        const password = passwordInput.value.trim();

        if (!login || !password) {
            statusDiv.textContent = "⚠️ Заполните логин и пароль";
            statusDiv.style.color = "orange";
            return;
        }

        statusDiv.textContent = "⏳ Включение тестового режима...";
        statusDiv.style.color = "blue";

        ws.send(JSON.stringify({
            action: "test_account",
            account_login: login,
            account_password: password
        }));

        testAccountActive = true;
        statusDiv.textContent = "✅ Тестовый режим включен";
        statusDiv.style.color = "green";
        btn.textContent = "Выйти из тестового режима"; // ← теперь работает

    } else {
        // Выключаем тестовый режим
        statusDiv.textContent = "⏳ Выключение тестового режима...";
        statusDiv.style.color = "blue";
        loadTabSettings("account");
        ws.send(JSON.stringify({
            action: "cancel_test_account"
        }));

        testAccountActive = false;
        statusDiv.textContent = "✅ Тестовый режим отключен";
        statusDiv.style.color = "green";
        btn.textContent = "Включить тестовый режим"; // ← теперь тоже работает
    }
});