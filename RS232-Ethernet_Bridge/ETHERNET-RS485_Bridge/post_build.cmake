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
message(STATUS "Создаём OTA пакет: ${OTA_PACKAGE}")

# === Создание файла склейки ===
execute_process(
    COMMAND ${CMAKE_COMMAND} -E echo_append "" # пустая команда, чтобы избежать ошибок
)

# Генерация файла через Python скрипт прямо в post-build
execute_process(
    COMMAND python -c "
import struct
data_file = r'${DATA_BIN}'
fw_file   = r'${ORIGINAL_BIN}'
out_file  = r'${OTA_PACKAGE}'
with open(data_file,'rb') as f:
    data_bytes = f.read()
with open(fw_file,'rb') as f:
    fw_bytes = f.read()
with open(out_file,'wb') as f:
    f.write(struct.pack('<I', len(data_bytes)))
    f.write(data_bytes)
    f.write(struct.pack('<I', len(fw_bytes)))
    f.write(fw_bytes)
print('OTA package created:', out_file)
"
    RESULT_VARIABLE OTA_RESULT
)

if(NOT OTA_RESULT EQUAL 0)
    message(FATAL_ERROR "Ошибка создания OTA пакета")
else()
    message(STATUS "OTA пакет успешно создан: ${OTA_PACKAGE}")
endif()

# === Копирование FW в отдельный файл для загрузки без DATA (старый механизм) ===
set(NEW_BIN "${CMAKE_BINARY_DIR}/${FW_NAME}")
message(STATUS "Копируем ${ORIGINAL_BIN} -> ${NEW_BIN}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy "${ORIGINAL_BIN}" "${NEW_BIN}"
    RESULT_VARIABLE COPY_RESULT
)
if(NOT COPY_RESULT EQUAL 0)
    message(FATAL_ERROR "Ошибка копирования файла прошивки")
endif()


message(STATUS "post-build скрипт завершён")


#[ 4 байта ]  data_len (uint32_t, LE)
#[ N байт ]  data.bin  (LittleFS)
#[ 4 байта ]  fw_len   (uint32_t, LE)
#[ M байт ]  firmware.bin (app)