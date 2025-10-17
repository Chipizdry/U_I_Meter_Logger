

document.addEventListener("DOMContentLoaded", () => {
    const form = document.getElementById("wifiForm");

    form.addEventListener("submit", async (e) => {
        e.preventDefault(); // отключаем стандартное поведение формы

        const formData = new FormData(form);
        const params = new URLSearchParams();

        for (const [key, value] of formData.entries()) {
            params.append(key, value);
        }

        try {
            const response = await fetch("/save_settings", {
                method: "POST",
                headers: {
                    "Content-Type": "application/x-www-form-urlencoded"
                },
                body: params.toString()
            });

            if (response.ok) {
                const text = await response.text();
                alert(text); // показываем сообщение об успехе
            } else {
                const text = await response.text();
                alert(`Ошибка: ${text}`);
            }
        } catch (err) {
            alert(`Ошибка соединения: ${err}`);
        }
    });
});


document.getElementById('loginForm').addEventListener('submit', async (e) => {
    e.preventDefault();

    const data = new URLSearchParams(new FormData(e.target));
    const res = await fetch('/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: data.toString()
    });

    if (res.ok) {
        const json = await res.json();
        localStorage.setItem('auth_token', json.token);
        alert('Авторизация успешна ✅');
    } else {
        const text = await res.text();
        alert('Ошибка входа 💩: ' + text);
    }
});