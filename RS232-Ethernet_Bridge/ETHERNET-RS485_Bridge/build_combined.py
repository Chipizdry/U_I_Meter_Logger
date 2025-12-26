

import os
import sys
import shutil
import subprocess

APP_BIN  = "build/ETHERNET_RS485_Bridge.bin"
LFS_DIR  = "main/data"
FS_BIN   = "build/littlefs.bin"
COMBINED = "build/combined.bin"

FS_MARKER = b"FS-CONTENT"
FS_SIZE   = 0x60000  # 384 KB

# Путь к mklittlefs.exe (ВАЖНО — в кавычках)
MKLITTLEFS = r'D:\GitHub\U_I_Meter_Logger\RS232-Ethernet_Bridge\ETHERNET-RS485_Bridge\mklittlefs\mklittlefs.exe'

print("Создаем бинарник LittleFS...")

os.makedirs("build", exist_ok=True)

if os.path.exists(FS_BIN):
    os.remove(FS_BIN)

cmd = (
    f'"{MKLITTLEFS}" '
    f'-c "{LFS_DIR}" '
    f'-b 4096 '
    f'-p 256 '
    f'-s {FS_SIZE} '
    f'"{FS_BIN}"'
)

print("CMD:", cmd)
ret = subprocess.call([
    MKLITTLEFS,
    "-c", LFS_DIR,
    "-b", "4096",
    "-p", "256",
    "-s", hex(FS_SIZE),
    FS_BIN
])
if ret != 0:
    print("❌ Ошибка создания FS бинарника")
    sys.exit(1)

print("Создаем combined.bin...")

with open(COMBINED, "wb") as out:
    with open(APP_BIN, "rb") as f:
        shutil.copyfileobj(f, out)
    out.write(FS_MARKER)
    with open(FS_BIN, "rb") as f:
        shutil.copyfileobj(f, out)

print(f"✅ Combined файл создан: {COMBINED}")
