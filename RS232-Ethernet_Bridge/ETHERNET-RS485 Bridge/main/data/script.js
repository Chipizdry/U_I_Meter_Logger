

document.addEventListener("DOMContentLoaded", () => {
   
   
    const eye = document.getElementById("eyeIcon");
    const token = sessionStorage.getItem('auth_token');
    updateEyeIcon(eye, false); // false = закрытый глаз
    initAllEyeIcons();

    

    ipFields.forEach(id => {
        const field = document.getElementById(id);
        if (field) applyIPMask(field);
    });


    dhcpCheckbox.addEventListener("change", updateIPFieldsState);
    
    if (token) {
        fetchSettings();
        initWebSocket();  

        showMainScreen();
       
        if (activeTab) {
        applySettingsToForm(activeTab, data);
        }
        loadTabSettings(activeTab);
    } else {
        showLogin();
    }
    

});



async function saveUARTSettings() {
   
    const token = sessionStorage.getItem("auth_token");
    const mode = parseInt(document.querySelector('input[name="uart-mode"]:checked')?.value || 0);
    const payload = {
        mode: mode,
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
    } 
    
    else if (res.status === 401) {
            sessionStorage.removeItem("auth_token");
            showTokenExpiredModal();
            return;
    }

    else {
        alert('Ошибка при сохранении UART настроек ❌');
    }
}



async function saveWiFiSettings() {
    const token = sessionStorage.getItem("auth_token");

    const wifiForm = document.getElementById("wifiForm");

    const payload = {
        mode: parseInt(document.getElementById("wifiMode").value),

        sta_ssid: wifiForm.querySelector("[name='sta-ssid']").value,
        sta_password: wifiForm.querySelector("[name='sta-password']").value,

        ap_ssid: wifiForm.querySelector("[name='ap-ssid']").value,
        ap_password: wifiForm.querySelector("[name='ap-password']").value,
        ap_channel: parseInt(wifiForm.querySelector("[name='ap-channel']").value),

        ip: wifiForm.querySelector("[name='ip']").value,
        mask: wifiForm.querySelector("[name='mask']").value,
        gateway: wifiForm.querySelector("[name='gateway']").value,
        dns: wifiForm.querySelector("[name='dns']").value,

        dhcp_enabled: wifiForm.querySelector("[name='dhcp']").checked ? 1 : 0
    };

    const res = await fetch('/save_settings/wifi', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            'Authorization': 'Bearer ' + token
        },
        body: JSON.stringify(payload)
    });

    if (res.ok) {
        alert('Wi-Fi настройки сохранены ✅');
    } 
    else if (res.status === 401) {
        sessionStorage.removeItem("auth_token");
        showTokenExpiredModal();
        return;
    }
    else {
        alert('Ошибка при сохранении Wi-Fi настроек ❌');
    }
}




let testAccountActive = false; // флаг тестового режима


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

            // Mode
            const modeField = document.getElementById("wifiMode");
            if (modeField) modeField.value = String(wifi.mode);

            // STA
            document.querySelector("[name='sta-ssid']").value = wifi.sta_ssid || "";
            document.querySelector("[name='sta-password']").value = wifi.sta_password || "";

            // AP
            document.querySelector("[name='ap-ssid']").value = wifi.ap_ssid || "";
            document.querySelector("[name='ap-password']").value = wifi.ap_password || "";
            document.querySelector("[name='ap-channel']").value = wifi.ap_channel || 1;

            // LAN
            document.querySelector("[name='ip']").value = wifi.ip || "";
            document.querySelector("[name='mask']").value = wifi.mask || "";
            document.querySelector("[name='gateway']").value = wifi.gateway || "";
            document.querySelector("[name='dns']").value = wifi.dns || "";

            // DHCP
            document.querySelector("[name='dhcp']").checked = !!wifi.dhcp_enabled;
            updateWiFiVisibility();
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
            document.querySelector("[name='node-name']").value = account.node_name || "";
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


const btn = document.getElementById("testAccountBtn"); 

btn.addEventListener("click", () => {
    const loginInput = document.querySelector("input[name='account-login']");
    const passwordInput = document.querySelector("input[name='account-password']");
    const nodeInput = document.querySelector("input[name='node-name']");
    const statusDiv = document.getElementById("ws-status");

    if (!testAccountActive) {
        // Включаем тестовый режим
        const login = loginInput.value.trim();
        const password = passwordInput.value.trim();
        const nodeName = nodeInput.value.trim();

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
            account_password: password ,
            node_name: nodeName
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


document.getElementById("firmwareFile").addEventListener("change", function () {
    const nameBox = document.getElementById("fileName");
    if (this.files.length > 0) {
        nameBox.textContent = this.files[0].name;
    } else {
        nameBox.textContent = "Файл не выбран";
    }
});

async function rebootDevice() {

    const token = sessionStorage.getItem("auth_token");

    const res = await fetch('/reboot', {
        method: 'POST',
        headers: {
            'Authorization': 'Bearer ' + token
        }
    });

    if (res.ok) {
        const text = await res.text();      // читаем ответ от бэка
        alert("Ответ устройства: " + text); // выводим пользователю
    } 
    
    else if (res.status === 401) {
        sessionStorage.removeItem("auth_token");
        showTokenExpiredModal();
        return;
    } 
    
    else {
        alert("Ошибка: " + res.status + " ❌");
    }
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

            // === Диагностика ===
            if (tabId === "diagnostics") {
                diagnosticsOn();
            } else {
                diagnosticsOff();
            }

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



function updateWiFiVisibility() {
    const mode = parseInt(document.getElementById("wifiMode").value);

    const staFields = document.querySelectorAll(".wifi-sta");
    const apFields  = document.querySelectorAll(".wifi-ap");
    const lanFields = document.querySelectorAll(".wifi-lan");

    // Скрыть всё
    staFields.forEach(el => el.classList.add("hidden"));
    apFields.forEach(el => el.classList.add("hidden"));
    lanFields.forEach(el => el.classList.add("hidden"));

    switch (mode) {
        case 1: // STA
            staFields.forEach(el => el.classList.remove("hidden"));
            break;

        case 2: // AP
            apFields.forEach(el => el.classList.remove("hidden"));
            break;

        case 3: // APSTA
            staFields.forEach(el => el.classList.remove("hidden"));
            apFields.forEach(el => el.classList.remove("hidden"));
            break;

        case 0: // OFF
        default:
            // всё скрыто
            break;
    }
}
 document.getElementById("wifiMode").addEventListener("change", updateWiFiVisibility);



 
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
            initWebSocket();
           // Показ успешного сообщения
           loginStatus.textContent = "Вход выполнен";
           loginStatus.style.color = "#4CAF50"; // зелёный
           loginStatus.classList.remove("hidden");
          // fetchSettings();
            setTimeout(async () => {
                 fetchSettings();
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
            window.allSettings = data; // Сохраняем глобально
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

               // === Автоматически применяем настройки на активную вкладку ===
          const activeTab = document.querySelector(".menu-item.active")?.dataset.tab || "account";
          loadTabSettings(activeTab);
        return data; 


        } catch (err) {
            alert("Ошибка соединения при получении настроек: " + err);
        }
    }



    // === Функции отображения ===
    function showMainScreen() {
        loginForm.classList.add("hidden");
        mainScreen.classList.remove("hidden");
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


// === Включение / выключение DHCP ===
    function updateIPFieldsState() {
        const disabled = dhcpCheckbox.checked;
        ipFields.forEach(id => {
            const field = document.getElementById(id);
            field.disabled = disabled;
            field.style.opacity = disabled ? "0.6" : "1.0";
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
 


function addDiagRow(text) {
    // ожидаем строку вида: "PI30 update : 1950"
    const match = text.match(/^(.+?)\s*:\s*(\d+)/);
    if (!match) return;

    const type = match[1];
    const value = parseInt(match[2], 10);

    const time = new Date().toLocaleTimeString();

    const row = document.createElement("div");
    row.className = "diag-row";

    const valueClass =
        value > 5000 ? "bad" :
        value > 2500 ? "warn" : "";

    row.innerHTML = `
        <span class="diag-type">${type}</span>
        <span class="diag-value ${valueClass}">${value}</span>
        <span class="diag-time">${time}</span>
    `;

    diagList.prepend(row);

    // оставляем только последние 10 строк
    while (diagList.children.length > DIAG_MAX_ROWS) {
        diagList.removeChild(diagList.lastChild);
    }
}


