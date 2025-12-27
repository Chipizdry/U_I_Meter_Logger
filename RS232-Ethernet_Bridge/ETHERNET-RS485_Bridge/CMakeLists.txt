


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

# === Дата и время сборки ===
string(TIMESTAMP BUILD_DATE "%Y-%m-%d_%H-%M-%S")

# === Имя прошивки ===
set(FW_NAME "COR-Bridge_${BUILD_NUMBER}_${BUILD_DATE}.bin")
message(STATUS "FW_NAME = ${FW_NAME}")

add_definitions(-DBUILD_NUMBER=${BUILD_NUMBER})
add_definitions(-DBUILD_DATE="${BUILD_DATE}")

# === ESP-IDF ===
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ETHERNET_RS485_Bridge)

message(STATUS "Build number: ${BUILD_NUMBER} (${BUILD_DATE})")

# === Переименование прошивки после сборки ===
add_custom_target(post_build ALL
    COMMAND ${CMAKE_COMMAND} 
        -DPROJECT_NAME=${PROJECT_NAME}
        -DBUILD_NUMBER=${BUILD_NUMBER}
        -DFW_NAME=${FW_NAME}
        -DCMAKE_BINARY_DIR=${CMAKE_BINARY_DIR}
        -DCMAKE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/post_build.cmake"
    DEPENDS app
    COMMENT "Переименование прошивки после сборки"
    VERBATIM
)


