# Перевод консоли Windows в UTF-8
if(WIN32)
    execute_process(COMMAND chcp 65001)
endif()

message(STATUS "Выполняется post-build скрипт")
message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")
message(STATUS "BUILD_NUMBER = ${BUILD_NUMBER}")
message(STATUS "FW_NAME = ${FW_NAME}")
message(STATUS "CMAKE_BINARY_DIR = ${CMAKE_BINARY_DIR}")

# === Путь к оригинальной прошивке ===
set(ORIGINAL_BIN "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin")

if(NOT EXISTS "${ORIGINAL_BIN}")
    set(FIRMWARE_PATHS
        "${CMAKE_BINARY_DIR}/ETHERNET_RS485_Bridge.bin"
        "${CMAKE_BINARY_DIR}/ethernet_rs485_bridge.bin"
        "${CMAKE_BINARY_DIR}/app.bin"
    )

    foreach(path IN LISTS FIRMWARE_PATHS)
        if(EXISTS "${path}")
            set(ORIGINAL_BIN "${path}")
            message(STATUS "Найден файл прошивки: ${ORIGINAL_BIN}")
            break()
        endif()
    endforeach()
endif()

if(NOT EXISTS "${ORIGINAL_BIN}")
    message(FATAL_ERROR "Не найден файл прошивки")
endif()

# === Путь к LittleFS образу (DATA) ===
set(DATA_BIN "${CMAKE_BINARY_DIR}/storage.bin")

if(NOT EXISTS "${DATA_BIN}")
    message(FATAL_ERROR "Не найден LittleFS образ: storage.bin")
endif()

# === Имя итогового OTA пакета ===
set(OTA_PACKAGE "${CMAKE_BINARY_DIR}/OTA_PACKAGE_${BUILD_NUMBER}.bin")

message(STATUS "")
message(STATUS "========================================")
message(STATUS "          OTA PACKAGE BUILD")
message(STATUS "========================================")
message(STATUS "DATA file = ${DATA_BIN}")
message(STATUS "FW   file = ${ORIGINAL_BIN}")
message(STATUS "OTA  file = ${OTA_PACKAGE}")
message(STATUS "========================================")

# === Создание OTA пакета ===
execute_process(
    COMMAND python -c "
import struct
import os

data_file = r'${DATA_BIN}'
fw_file   = r'${ORIGINAL_BIN}'
out_file  = r'${OTA_PACKAGE}'

with open(data_file, 'rb') as f:
    data_bytes = f.read()

with open(fw_file, 'rb') as f:
    fw_bytes = f.read()

data_len = len(data_bytes)
fw_len = len(fw_bytes)

# OTA формат:
#
# [4 байта] DATA length
# [N байт]  DATA
# [4 байта] FW length
# [M байт]  FW

data_header = struct.pack('<I', data_len)
fw_header   = struct.pack('<I', fw_len)

with open(out_file, 'wb') as f:
    f.write(data_header)
    f.write(data_bytes)
    f.write(fw_header)
    f.write(fw_bytes)

ota_size = 4 + data_len + 4 + fw_len

print('')
print('========================================')
print('           OTA PACKAGE INFO')
print('========================================')
print(f'DATA size       : {data_len} bytes')
print(f'DATA size       : {data_len / 1024:.2f} KB')
print(f'DATA size       : {data_len / 1024 / 1024:.2f} MB')
print('')
print(f'FW size         : {fw_len} bytes')
print(f'FW size         : {fw_len / 1024:.2f} KB')
print(f'FW size         : {fw_len / 1024 / 1024:.2f} MB')
print('')
print(f'DATA header     : {data_header.hex(\" \")}')
print(f'FW header       : {fw_header.hex(\" \")}')
print('')
print(f'OTA total size  : {ota_size} bytes')
print(f'OTA total size  : {ota_size / 1024:.2f} KB')
print(f'OTA total size  : {ota_size / 1024 / 1024:.2f} MB')
print('')
print(f'OTA file actual : {os.path.getsize(out_file)} bytes')
print('')
print('OTA package created:', out_file)
print('========================================')
"
    RESULT_VARIABLE OTA_RESULT
)

if(NOT OTA_RESULT EQUAL 0)
    message(FATAL_ERROR "Ошибка создания OTA пакета")
else()
    message(STATUS "")
    message(STATUS "OTA пакет успешно создан:")
    message(STATUS "${OTA_PACKAGE}")
endif()

# === Копирование FW в отдельный файл для загрузки без DATA ===
set(NEW_BIN "${CMAKE_BINARY_DIR}/${FW_NAME}")

message(STATUS "Копируем ${ORIGINAL_BIN} -> ${NEW_BIN}")

execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy
            "${ORIGINAL_BIN}"
            "${NEW_BIN}"
    RESULT_VARIABLE COPY_RESULT
)

if(NOT COPY_RESULT EQUAL 0)
    message(FATAL_ERROR "Ошибка копирования файла прошивки")
endif()

message(STATUS "")
message(STATUS "post-build скрипт завершён")