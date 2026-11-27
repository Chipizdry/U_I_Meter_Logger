#ifndef LITTLEFS_MANAGER_H
#define LITTLEFS_MANAGER_H

#include "esp_err.h"

// Инициализация LittleFS
esp_err_t littlefs_init(void);

// Список файлов
void littlefs_list_files(void);

// Запись в файл
esp_err_t littlefs_write_file(const char *path, const char *data);

// Чтение из файла
esp_err_t littlefs_read_file(const char *path);

// Удаление файла
esp_err_t littlefs_delete_file(const char *path);

#endif // LITTLEFS_MANAGER_H
