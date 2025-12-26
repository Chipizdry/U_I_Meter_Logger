


# post_build.cmake
message(STATUS "Выполняется post-build скрипт")
message(STATUS "PROJECT_NAME = ${PROJECT_NAME}")
message(STATUS "CMAKE_BINARY_DIR = ${CMAKE_BINARY_DIR}")

# Выведем список всех файлов в каталоге сборки для отладки
execute_process(
    COMMAND ${CMAKE_COMMAND} -E echo "Содержимое каталога сборки:"
    COMMAND ${CMAKE_COMMAND} -E echo "----------------------------------------"
)

if(WIN32)
    execute_process(
        COMMAND cmd /c dir /b "${CMAKE_BINARY_DIR}\\*.bin"
        OUTPUT_VARIABLE BIN_FILES
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
else()
    execute_process(
        COMMAND ls -la "${CMAKE_BINARY_DIR}/*.bin"
        OUTPUT_VARIABLE BIN_FILES
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

message(STATUS "Найденные .bin файлы:")
message(STATUS "${BIN_FILES}")

# Ищем файл прошивки
set(FIRMWARE_PATHS
    "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin"
    "${CMAKE_BINARY_DIR}/ETHERNET_RS485_Bridge.bin"
    "${CMAKE_BINARY_DIR}/ethernet_rs485_bridge.bin"
    "${CMAKE_BINARY_DIR}/app.bin"
    "${CMAKE_BINARY_DIR}/bootloader/bootloader.bin"
    "${CMAKE_BINARY_DIR}/partition_table/partition-table.bin"
)

set(ORIGINAL_BIN "")
foreach(path IN LISTS FIRMWARE_PATHS)
    if(EXISTS "${path}")
        set(ORIGINAL_BIN "${path}")
        message(STATUS "Найден файл прошивки: ${ORIGINAL_BIN}")
        break()
    endif()
endforeach()

if(NOT ORIGINAL_BIN)
    # Пробуем найти любой .bin файл
    file(GLOB_RECURSE ALL_BIN_FILES "${CMAKE_BINARY_DIR}/*.bin")
    if(ALL_BIN_FILES)
        list(GET ALL_BIN_FILES 0 ORIGINAL_BIN)
        message(STATUS "Найден .bin файл (первый из списка): ${ORIGINAL_BIN}")
    else()
        message(FATAL_ERROR "Не найден файл прошивки. Проверенные пути: ${FIRMWARE_PATHS}")
    endif()
endif()

# Читаем номер сборки из файла
set(BUILD_NUMBER_FILE "${CMAKE_SOURCE_DIR}/build_number.txt")
if(EXISTS ${BUILD_NUMBER_FILE})
    file(READ ${BUILD_NUMBER_FILE} BUILD_NUMBER)
    string(STRIP "${BUILD_NUMBER}" BUILD_NUMBER)
else()
    set(BUILD_NUMBER 1)
endif()

string(TIMESTAMP BUILD_DATE "%Y-%m-%d_%H-%M-%S")
set(FW_NAME "COR-Bridge_${BUILD_NUMBER}_${BUILD_DATE}.bin")
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

