


import os
import sys
import shutil
import subprocess

if len(sys.argv) > 1:
    FW_NAME = sys.argv[1]
else:
    FW_NAME = "COR-Bridge.bin"

APP_BIN  = "build/ETHERNET_RS485_Bridge.bin"
LFS_DIR  = "main/data"
FS_BIN   = "build/littlefs.bin"
COMBINED = f"build/{FW_NAME}"

FS_MARKER = FW_NAME.encode("utf-8")
FS_SIZE   = 0x60000  # 384 KB
MKLITTLEFS = r'D:\GitHub\U_I_Meter_Logger\RS232-Ethernet_Bridge\ETHERNET-RS485_Bridge\mklittlefs\mklittlefs.exe'

os.makedirs("build", exist_ok=True)
if os.path.exists(FS_BIN):
    os.remove(FS_BIN)

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

with open(COMBINED, "wb") as out:
    with open(APP_BIN, "rb") as f:
        shutil.copyfileobj(f, out)
    out.write(FS_MARKER)
    with open(FS_BIN, "rb") as f:
        shutil.copyfileobj(f, out)

print(f"✅ Combined файл создан: {COMBINED}")


