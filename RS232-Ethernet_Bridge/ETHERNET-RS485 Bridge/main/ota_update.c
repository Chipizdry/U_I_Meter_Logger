


#include "ota_update.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include <stdlib.h>



#define BOUNDARY_SUFFIX "\r\n"

#define BOUNDARY_PREFIX "------WebKitFormBoundary"

#define OTA_CHUNK_SIZE 2048
static const char *TAG = "OTA";

static int total_received = 0;


static int total_size=0;

static bool ota_started = false;
static esp_ota_handle_t ota_handle=0; 

void ota_init(void) {
    ota_started = false;
    total_received = 0;
    ota_handle = 0;
}


 
esp_err_t ota_post_handler(httpd_req_t *req) {
    char *buffer = malloc(OTA_CHUNK_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate buffer");
        return ESP_FAIL;
    }

    int received = 0;  // Количество полученных байт
   
    char content_type[256] = {0};
    char *boundary = NULL;
   
   

    esp_err_t ret;
    if(ota_started==false){
	

        const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!ota_partition) {
            ESP_LOGE(TAG, "OTA partition not found");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA partition not found");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Found OTA partition: %s, size: %lu", ota_partition->label, (unsigned long)ota_partition->size);

    ret = esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    ota_started=true;
    if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed! err = %d", ret);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
    free(buffer);
    return ESP_FAIL;
    } else {
    ESP_LOGI(TAG, "esp_ota_begin succeeded, OTA handle initialized.");
      }
    }
    
     const esp_partition_t *running_partition = esp_ota_get_running_partition();
  

    ESP_LOGI(TAG, "Request method: %s", http_method_str(req->method));
    ESP_LOGI(TAG, "Request URI: %s", req->uri);
    ESP_LOGI(TAG, "Content Length: %d", req->content_len);
    ESP_LOGI(TAG, "Running partition: %s", running_partition->label);
    // Читаем данные из запроса
httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));

// Получаем заголовок Content-Type  
ESP_LOGI(TAG, "Received Content-Type: %s", content_type);

// Находим boundary в заголовке Content-Type
boundary = strstr(content_type, "boundary=");
if (!boundary) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary found");
    free(buffer);
    return ESP_FAIL;
}
boundary += strlen("boundary="); // Пропускаем "boundary="
ESP_LOGI(TAG, "Boundary: %s", boundary);

ESP_LOGI(TAG, "---- DEBUG FIRST CHUNK ----");
ESP_LOG_BUFFER_CHAR(TAG, buffer, received);
ESP_LOG_BUFFER_HEX(TAG, buffer, received);
ESP_LOGI(TAG, "----------------------------");

// Читаем тело запроса и обрабатываем чанки

while ((received = httpd_req_recv(req, buffer, sizeof(buffer) - 1)) > 0) {
    buffer[received] = '\0';  // Завершаем строку для безопасности
 
    char *data_start = buffer;
    char *boundary_pos = strstr(data_start, boundary);
    
   // Обрабатываем части, пока находим boundary
while (boundary_pos) {
    // Пропускаем boundary и ищем следующий блок данных
    data_start = boundary_pos + strlen(boundary);
    size_t total_length = received - (data_start - buffer);
    // Ищем заголовок Content-Disposition
 //   char *content_disp = strstr(data_start, "Content-Disposition");
    char *content_disp = memmem(data_start, total_length, "Content-Disposition", strlen("Content-Disposition"));
      ESP_LOGI(TAG, "Content-Disposition:%s",content_disp);  // Выводим данные как строку
    if (!content_disp) {
        ESP_LOGE(TAG, "Content-Disposition not found");
        break; // Если не найден Content-Disposition, завершаем
    }
		// Находим "name=\"totalSize\""
		char *total_size_start = memmem(data_start, total_length, "name=\"totalSize\"", strlen("name=\"totalSize\""));
		if (!total_size_start) {
		    ESP_LOGE(TAG, "Total size field not found");
		    break;
		}
		
		// Смещаем указатель до конца строки "name=\"totalSize\""
		total_size_start += strlen("name=\"totalSize\"");
		
		// Пропускаем любые пробелы, символы новой строки или символы перевода строки
		while (*total_size_start == '\n' || *total_size_start == '\r' || *total_size_start == ' ') {
		    total_size_start++;
		}
		
		// Теперь total_size_start указывает на значение totalSize, считываем его
		char total_size_str[16] = {0}; // Для хранения значения totalSize
		int i = 0;
		while (*total_size_start >= '0' && *total_size_start <= '9' && i < sizeof(total_size_str) - 1) {
		    total_size_str[i++] = *total_size_start++;
		}
		total_size_str[i] = '\0'; // Завершаем строку
		
		// Преобразуем строку в целое число
		total_size = atoi(total_size_str);
		
		ESP_LOGI(TAG, "Total Size: %d", total_size);
     // Ищем конец заголовка Content-Disposition
    char *content_end = memmem(content_disp, total_length - (content_disp - data_start), "application/octet-stream", strlen("application/octet-stream"));
   
    if (!content_end) {
        ESP_LOGE(TAG, "End of Content-Disposition not found");
        break;
    }
    ESP_LOGI(TAG, "End of Content-Disposition found");

    // Устанавливаем конец заголовка
    content_end += strlen("application/octet-stream"); // Пропускаем "application/octet-stream"

    // Находим начало данных (после заголовка)
    char *data_start = content_end + 4; // Пропускаем следующий символ (который, скорее всего, '\n' или '\r')
   
    // Находим следующую границу (boundary) для определения конца данных
    
    size_t data_length = received - (data_start - buffer); // Остаток данных
    char *boundary_position = (char*)memmem(data_start, total_length, boundary, strlen(boundary));

     // Если граница найдена
    if (boundary_position) {
        data_length = boundary_position - data_start-4;  // Длина данных до границы
        ESP_LOGI(TAG, "Binary data length: %d bytes", data_length);

        // Выводим двоичные данные
        ESP_LOG_BUFFER_HEX(TAG, data_start, data_length);  // Выводим двоичные данные в лог
           // Проверка инициализации OTA
          //  if (ota_handle == NULL) { 
            if (ota_handle ==0) {
            ESP_LOGE(TAG, "OTA handle is null, something went wrong with esp_ota_begin.");
            free(buffer);
              return ESP_FAIL;}



        // Записываем данные в OTA
                ret = esp_ota_write(ota_handle, data_start, data_length);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_write failed! err = %d", ret);
                    esp_ota_end(ota_handle); // Завершаем OTA
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_write failed");
                    free(buffer); 
                    return ESP_FAIL;
                }
        
        
    } else {
        ESP_LOGE(TAG, "Boundary not found");
        break;  // Завершаем, если не найдено следующее boundary
    }
    
    total_received +=data_length;
     ESP_LOGI(TAG, "OTA update: %d bytes", total_received);
     // Выход из внутреннего цикла при нахождении границы
            break;
   
}
// Очищаем буфер после обработки
    memset(buffer, 0, sizeof(buffer));
}
   
    if (received == 0) {	
    // Обработка завершена, данные полностью переданы
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OTA update completed", HTTPD_RESP_USE_STRLEN);
}
     if( total_received==total_size){
  // Обработка завершена, данные полностью переданы
       esp_err_t end_err = esp_ota_end(ota_handle);
    if (end_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        ESP_LOGI(TAG, "OTA Update ERROR.....");
    } else {
        httpd_resp_send(req, "OTA Update Success", HTTPD_RESP_USE_STRLEN);
          ESP_LOGI(TAG, "OTA Update Success.Now rebooting.....");
  // Определяем следующий раздел для загрузки
    const esp_partition_t *next_partition = (running_partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) ?
                                            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL) :
                                            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);

    if (next_partition != NULL) {
        ESP_LOGI(TAG, "Next partition: %s", next_partition->label);

        // Устанавливаем новый раздел для загрузки при следующем перезапуске
        esp_err_t err = esp_ota_set_boot_partition(next_partition);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Set next partition as boot");
        } else {
            ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        }
        free(buffer);
        // Перезагрузка для загрузки нового приложения
        esp_restart();
    } else {
        ESP_LOGE(TAG, "Next partition not found!");
        free(buffer);
    }  esp_restart();
    }}
    return ESP_OK;
    
}



