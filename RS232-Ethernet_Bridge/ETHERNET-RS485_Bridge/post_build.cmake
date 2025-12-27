


# post_build.cmake
message(STATUS "Выполняется post-build скрипт")
message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")
message(STATUS "BUILD_NUMBER = ${BUILD_NUMBER}")
message(STATUS "FW_NAME = ${FW_NAME}")
message(STATUS "CMAKE_BINARY_DIR = ${CMAKE_BINARY_DIR}")

# Используем переданные переменные напрямую
set(ORIGINAL_BIN "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin")

if(NOT EXISTS "${ORIGINAL_BIN}")
    # Если нет файла с именем проекта, ищем другие варианты
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

# Используем уже готовое имя файла из FW_NAME
set(NEW_BIN "${CMAKE_BINARY_DIR}/${FW_NAME}")

message(STATUS "Копируем ${ORIGINAL_BIN} -> ${NEW_BIN}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy "${ORIGINAL_BIN}" "${NEW_BIN}"
    RESULT_VARIABLE result
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "Ошибка копирования файла")
else()
    message(STATUS "Прошивка успешно сохранена как: ${FW_NAME}")
endif()


