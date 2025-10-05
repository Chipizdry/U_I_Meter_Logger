



document.getElementById("wifiForm").addEventListener("submit", function(e) {
    e.preventDefault();
    const formData = new FormData(e.target);
    fetch("/set_wifi", {
        method: "POST",
        body: JSON.stringify({
            ssid: formData.get("ssid"),
            password: formData.get("password")
        }),
        headers: {
            "Content-Type": "application/json"
        }
    })
    .then(r => r.text())
    .then(alert)
    .catch(console.error);
});




document.getElementById('wifiForm').addEventListener('submit', async (e) => {
    e.preventDefault();

    const formData = new FormData(e.target);
    const data = {
        ssid: formData.get('ssid'),
        password: formData.get('password'),
        mode: formData.get('mode')
    };

    await fetch('/set_wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data)
    });

    alert("Настройки отправлены на ESP32");
});