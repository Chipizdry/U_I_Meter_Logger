

cmake_minimum_required(VERSION 3.16)

# === Автоинкремент номера сборки ===
set(BUILD_NUMBER_FILE "${CMAKE_SOURCE_DIR}/build_number.txt")
if(EXISTS ${BUILD_NUMBER_FILE})
    file(READ ${BUILD_NUMBER_FILE} BUILD_NUMBER)
else()
    set(BUILD_NUMBER 0)
endif()
string(STRIP "${BUILD_NUMBER}" BUILD_NUMBER)
math(EXPR BUILD_NUMBER "${BUILD_NUMBER} + 1")
file(WRITE ${BUILD_NUMBER_FILE} "${BUILD_NUMBER}")

# === ESP-IDF ===
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ETHERNET_RS485_Bridge)

# Добавляем post-build действие
add_custom_target(post_build ALL
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_SOURCE_DIR}/post_build.cmake"
    DEPENDS app
    COMMENT "Переименование прошивки после сборки"
    VERBATIM
)

