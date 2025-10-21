# === ESP-IDF CMake базовая часть ===
cmake_minimum_required(VERSION 3.16)

# === Автоинкремент номера сборки ===
set(BUILD_NUMBER_FILE "${CMAKE_SOURCE_DIR}/build_number.txt")

# Прочитать текущий номер (если файла нет — начать с 0)
if(EXISTS ${BUILD_NUMBER_FILE})
    file(READ ${BUILD_NUMBER_FILE} BUILD_NUMBER)
else()
    set(BUILD_NUMBER 0)
endif()

# Удаляем лишние пробелы/переводы строк
string(STRIP "${BUILD_NUMBER}" BUILD_NUMBER)

# Увеличиваем номер
math(EXPR BUILD_NUMBER "${BUILD_NUMBER} + 1")

# Сохраняем обратно в файл
file(WRITE ${BUILD_NUMBER_FILE} "${BUILD_NUMBER}")

# Передаём значение компилятору в виде макроса
add_definitions(-DBUILD_NUMBER=${BUILD_NUMBER})

# Также можем добавить дату/время компиляции (по желанию)
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")
add_definitions(-DBUILD_DATE="${BUILD_DATE}")

message(STATUS "🚀 Build number: ${BUILD_NUMBER} (${BUILD_DATE})")

# === Подключаем ESP-IDF систему сборки ===
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# === Имя проекта (оставь как есть, но без пробелов!) ===
# В названии проекта не должно быть пробелов — это ломает пути в build/
project(ETHERNET_RS485_Bridge)
