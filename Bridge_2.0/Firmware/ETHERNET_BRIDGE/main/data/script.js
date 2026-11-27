

document.addEventListener("DOMContentLoaded",  async() => { 
    const eye = document.getElementById("eyeIcon");
    const token = sessionStorage.getItem('auth_token');
    const dhcpCheckbox = document.getElementById("dhcp");
    const wifiDhcpCheckbox = document.getElementById("wifi_dhcp");
    updateEyeIcon(eye, false); 
    initAllEyeIcons();

        [...ethIpFields, ...wifiIpFields].forEach(id => {
        const field = document.getElementById(id);
        if (field) applyIPMask(field);
    });

        dhcpCheckbox.addEventListener("change", () => {
        console.log("ETH DHCP:", dhcpCheckbox.checked);
        updateEthIPFieldsState();
    });

    wifiDhcpCheckbox.addEventListener("change", () => {
        console.log("WiFi DHCP:", wifiDhcpCheckbox.checked);
        updateWifiIPFieldsState();
    });
    
    if (token) {
        initWebSocket();   
          // ЖДЁМ настройки
        const settings = await fetchSettings();
        if (!settings) return;
        showMainScreen();  
       
        if (activeTab) {
        applySettingsToForm(activeTab, settings);
        }
    } else {
        showLogin();
    }
    

});





function applySettingsToForm(tab, data) {
    switch (tab) {
        case "LAN": {
            const net = data.network || {};

            // Ethernet
            document.getElementById("ip").value = net.ip || "";
            document.getElementById("mask").value = net.mask || "";
            document.getElementById("gateway").value = net.gateway || "";
            document.getElementById("dns").value = net.dns || "";
            document.getElementById("dhcp").checked = !!net.dhcp_enabled;

            // Wi-Fi
            document.getElementById("wifi_ip").value = net.wifi_ip || "";
            document.getElementById("wifi_mask").value = net.wifi_mask || "";
            document.getElementById("wifi_gateway").value = net.wifi_gateway || "";
            document.getElementById("wifi_dns").value = net.wifi_dns || "";
            document.getElementById("wifi_dhcp").checked = !!net.wifi_dhcp_enabled;

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

            updateWiFiVisibility();
            break;
        }


        case "uart": {
            const uart = data.uart || {};

            const mode = uart.rs485_mode ? "1" : "0";

            // обычные поля
            document.getElementById("uart-baud").value = uart.baud ?? 9600;
            document.getElementById("uart-data-bits").value = uart.data_bits ?? 8;
            document.getElementById("uart-stop-bits").value = uart.stop_bits ?? 1;
            document.getElementById("uart-parity").value = uart.parity ?? 0;

            //сначала сбрасываем
            document.querySelectorAll("input[name='uart-mode']").forEach(r => {
                r.checked = false;
            });

            // ✅ применяем ПОСЛЕ рендера
            requestAnimationFrame(() => {
                const rb = document.querySelector(
                    `input[name='uart-mode'][value='${mode}']`
                );
                if (rb) rb.checked = true;
            });

            break;
        }

        case "user": {
            const user = data.user || {};
            document.querySelector("[name='user-login']").value = user.login || "";
            break;
        }
/*
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

*/
        case "account": {
            const account = data.user || {};
            document.querySelector("[name='account-login']").value = account.account_login || "";
            document.querySelector("[name='node-name']").value = account.node_name || "";
            // Обрабатываем статус так же,
            // как статус WebSocket/облака
            updateCloudStatus( account.status);
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
      //  statusDiv.textContent = "✅ Тестовый режим включен";
      //  statusDiv.style.color = "green";
        btn.textContent = "Выйти из тестового режима"; 

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
            // Загружаем настройки для конкретной вкладки
            await loadTabSettings(tabId);

            // === Диагностика ===
            if (tabId === "diagnostics") {
                diagnosticsOn();
            } else {
                diagnosticsOff();
            }

            document.getElementById(tabId).classList.add("active");

            // Закрываем меню на мобилке
            sideMenu.classList.remove("open");
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
                document.getElementById("dhcp").checked = !!net.dhcp_enabled;
                document.getElementById("wifi_dhcp").checked = !!net.wifi_dhcp_enabled;
                updateAllIPFieldsState();
            }
            
            const activeTab = document.querySelector(".menu-item.active")?.dataset.tab || "account";
            applySettingsToForm(activeTab, data);
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


function setFieldsDisabled(fieldIds, disabled) {
    fieldIds.forEach(id => {
        const field = document.getElementById(id);
        if (!field) return;

        field.disabled = disabled;

        // 💡 важно
        field.classList.toggle("ip-disabled", disabled);

        // 💡 чтобы required не ломал форму
        if (disabled) {
            field.removeAttribute("required");
        } else {
            field.setAttribute("required", "required");
        }
    });
}


function updateEthIPFieldsState() {
    const disabled = dhcpCheckbox.checked;
    setFieldsDisabled(ethIpFields, disabled);
}

function updateWifiIPFieldsState() {
    const disabled = wifiDhcpCheckbox.checked;
    setFieldsDisabled(wifiIpFields, disabled);
}

function updateAllIPFieldsState() {
    updateEthIPFieldsState();
    updateWifiIPFieldsState();
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
 

function setFirmwareUploadState(uploading) {

    const uploadButton = document.getElementById("uploadFirmwareBtn");
    const fileInput = document.getElementById("firmwareFile");

    // Кнопка "Выберите файл"
    const fileSelectButton = document.querySelector(
        ".file-input-box .file-btn"
    );

    // Кнопка "Перезагрузить устройство"
    const rebootButton = document.querySelector(
        "#system button[onclick='rebootDevice()']"
    );

    // Кнопка "Сбросить настройки"
    const factoryResetButton = document.querySelector(
        "#system button[onclick='FactoryDefaults()']"
    );

    // Все элементы, которые должны быть заблокированы во время OTA
    const otaControls = [
        uploadButton,
        fileSelectButton,
        fileInput,
        rebootButton,
        factoryResetButton
    ];

    otaControls.forEach(element => {
        if (!element) return;

        element.disabled = uploading;
        element.classList.toggle("ota-disabled", uploading);
    });


    // =====================================================
    // Кнопка загрузки прошивки
    // =====================================================

    if (uploadButton) {

        if (uploading) {

            uploadButton.dataset.originalText =
                uploadButton.textContent;

            uploadButton.textContent = "Загрузка...";
            uploadButton.classList.add("uploading");

        } else {

            uploadButton.textContent =
                uploadButton.dataset.originalText ||
                "Обновить прошивку";

            uploadButton.classList.remove("uploading");
        }
    }

}

async function uploadFirmware() {
    const fileInput = document.getElementById("firmwareFile");
    const status = document.getElementById("status");

    // Защита от повторного запуска
    const uploadButton = document.getElementById("uploadFirmwareBtn");

    if (uploadButton && uploadButton.disabled) {
        return;
    }

    if (!fileInput.files.length) {
        status.innerText = "Выберите файл прошивки!";
        return;
    }

    const file = fileInput.files[0];
    const totalSize = file.size;

    let offset = 0;
    let chunkNumber = 1;

    // ==========================================
    // БЛОКИРУЕМ UI НА ВЕСЬ ПЕРИОД OTA
    // ==========================================

    setFirmwareUploadState(true);

    status.innerText = "Запуск загрузки прошивки...";

    try {

        async function sendNextChunk(retry = 0) {

            const chunk = file.slice(
                offset,
                offset + CHUNK_SIZE
            );

            const ab = await chunk.arrayBuffer();

            const token = sessionStorage.getItem("auth_token");

            if (!token) {
                throw new Error("Authorization token not found");
            }

            const md5sum = md5(ab);

            console.log(
                `Chunk ${chunkNumber}: ${ab.byteLength} bytes, MD5=${md5sum}`
            );

            const form = new FormData();

            form.append("fileName", file.name);
            form.append("totalSize", totalSize);
            form.append("chunkNumber", chunkNumber);
            form.append("md5", md5sum);
            form.append("chunk",new Blob([ab]),"chunk.bin");

            const response = await fetch("/ota", {
                method: "POST",
                headers: {
                    "Authorization": `Bearer ${token}`
                },
                body: form
            });

            // ==========================================
            // TOKEN EXPIRED
            // ==========================================

            if (response.status === 401) {

                sessionStorage.removeItem("auth_token");

                showTokenExpiredModal();

                throw new Error("Authorization expired");
            }

            // ==========================================
            // MD5 ERROR
            // ==========================================

            if (response.status === 409) {

                if (retry < MAX_RETRIES) {

                    console.warn(
                        `MD5 mismatch → retry chunk ${chunkNumber}`
                    );

                    status.innerText =
                        `Retry chunk ${chunkNumber}...`;

                    return sendNextChunk(retry + 1);
                }

                let errorMessage =
                    "MD5 verification failed";

                try {
                    const json = await response.json();

                    if (json.message) {
                        errorMessage = json.message;
                    }

                } catch (e) {
                    // Ответ не JSON — оставляем стандартное сообщение
                }

                throw new Error(errorMessage);
            }

            // ==========================================
            // ДРУГАЯ HTTP ОШИБКА
            // ==========================================

            if (!response.ok) {

                let errorMessage =
                    `Upload failed (HTTP ${response.status})`;

                try {

                    const json = await response.json();

                    if (json.message) {
                        errorMessage = json.message;
                    }

                } catch (e) {
                    // Ответ не JSON
                }

                throw new Error(errorMessage);
            }

            // ==========================================
            // ПОСЛЕДНИЙ CHUNK
            // ==========================================

            if (offset + ab.byteLength >= totalSize) {

                let json = {};

                try {
                    json = await response.json();
                } catch (e) {
                    console.warn(
                        "Последний OTA ответ не является JSON"
                    );
                }

                console.log("OTA завершён:", json);

                status.innerText =
                    "✅ Прошивка загружена,устройство перезагружается...";

                showPopup({
                    type: "success",
                    title: "Обновлена прошивка",
                    message:
                        json.message ||
                        "Прошивка успешно загружена. Устройство перезагружается...",
                    timeout: 4000
                });

                // ВАЖНО:
                // здесь НЕ разблокируем кнопку.
                //
                // OTA завершился успешно,
                // устройство сейчас перезагрузится.
                //
                // finally() ниже выполнит разблокировку.

                return;
            }

            // ==========================================
            // ПЕРЕХОД К СЛЕДУЮЩЕМУ CHUNK
            // ==========================================

            offset += ab.byteLength;
            chunkNumber++;

            const percent =
                Math.round((offset / totalSize) * 100);

            status.innerText =
                `Загрузка прошивки: ${percent}%`;

            // ВАЖНО — await!
            await sendNextChunk(0);
        }

        // ==========================================
        // ЗАПУСК OTA
        // ==========================================

        await sendNextChunk(0);

    } catch (error) {

        console.error("OTA error:", error);

        status.innerText =
            "❌ Загрузка прошивки не удалась";

        showPopup({
            type: "error",
            title: "Ошибка OTA",
            message: error.message || "Ошибка загрузки прошивки",
            timeout: 5000
        });

    } finally {

        // ==========================================
        // ОБЯЗАТЕЛЬНО РАЗБЛОКИРУЕМ UI
        // ==========================================

        setFirmwareUploadState(false);
    }
}





function md5(arrayBuffer) {
    const bytes = new Uint8Array(arrayBuffer);
    const words = [];
    for (let i = 0; i < bytes.length; i++) {
        words[i >>> 2] |= bytes[i] << (24 - (i % 4) * 8);
    }
    const wordArray = CryptoJS.lib.WordArray.create(words, bytes.length);
    return CryptoJS.MD5(wordArray).toString();
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


document.querySelectorAll('input[name="netType"]').forEach(radio => {
    radio.addEventListener("change", () => {
        document.getElementById("ethernetBlock")
            .classList.toggle("hidden", radio.value !== "ethernet");

        document.getElementById("wifiBlock")
            .classList.toggle("hidden", radio.value !== "wifi");
    });
});



function showPopup({
    type = "info",
    title = "",
    message = "",
    timeout = 3000
}) {
    const container = document.getElementById("popup-container");
    if (!container) return;

    const popup = document.createElement("div");
    popup.className = `popup ${type}`;

    popup.innerHTML = `
        ${title ? `<h4>${title}</h4>` : ""}
        <div>${message}</div>
    `;

    container.appendChild(popup);

    if (timeout > 0) {
        setTimeout(() => {
            popup.style.opacity = "0";
            setTimeout(() => popup.remove(), 300);
        }, timeout);
    }

    return popup;
}


function updateCloudStatus(status) {

    const cloudEl = document.getElementById("ws-status");
    if (!cloudEl) return;

    switch (status) {

        case "connected":
            cloudEl.textContent = "☁️ Подключено к облаку";
            cloudEl.style.color = "green";
            break;

        case "authenticated":
            cloudEl.textContent = "✅ Авторизовано";
            cloudEl.style.color = "green";
            break;

        case "Connecting...":
            cloudEl.textContent = "⏳ Подключение к облаку...";
            cloudEl.style.color = "orange";
            break;

        case "disconnected":
            cloudEl.textContent = "⚠️ Отключено от облака";
            cloudEl.style.color = "red";
            break;

        case "error":
            cloudEl.textContent = "❌ Ошибка облака";
            cloudEl.style.color = "red";
            break;
        case "Auth error: Invalid credentials":
            cloudEl.textContent = "❌ Ошибка: неверные данные";
            cloudEl.style.color = "red";
            break;    

        default:
            cloudEl.textContent = status || "❔ Неизвестный статус";
            cloudEl.style.color = "blue";
            break;
    }
}