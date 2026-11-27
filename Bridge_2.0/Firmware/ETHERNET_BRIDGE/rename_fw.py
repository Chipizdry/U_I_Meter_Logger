


import sys
import os
import shutil

if len(sys.argv) < 2:
    print("Usage: rename_fw.py <new_fw_name>")
    sys.exit(1)

new_name = sys.argv[1]

# Путь к бинарнику прошивки, который сгенерировал ESP-IDF
bin_path = os.path.join("build", "ETHERNET_RS485_Bridge.bin")
if not os.path.exists(bin_path):
    print(f"❌ Файл {bin_path} не найден. Соберите прошивку сначала.")
    sys.exit(1)

# Новый путь
new_path = os.path.join("build", new_name)

# Копируем и удаляем старый
shutil.copy(bin_path, new_path)
os.remove(bin_path)

print(f"✅ Прошивка переименована: {new_path}")


