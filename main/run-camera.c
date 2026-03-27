#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <esp_http_server.h>
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_timer.h"
// --- ĐỊNH NGHĨA Ranh giới (Boundary) giữa các khung hình ---
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char *TAG = "ESP32-CAM";

// 1. THAY ĐỔI TÊN WIFI VÀ MẬT KHẨU CỦA BẠN Ở ĐÂY!
#define WIFI_SSID "TrinityX Phong Khach"
#define WIFI_PASS "12345678"

// Chân đèn Flash cực sáng của ESP32-CAM
#define FLASH_LED_GPIO 4 

// Cấu hình chân ESP32-CAM (AI-Thinker)
#define CAM_PIN_PWDN 32
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 0
#define CAM_PIN_SIOD 26
#define CAM_PIN_SIOC 27
#define CAM_PIN_D7 35
#define CAM_PIN_D6 34
#define CAM_PIN_D5 39
#define CAM_PIN_D4 36
#define CAM_PIN_D3 21
#define CAM_PIN_D2 19
#define CAM_PIN_D1 18
#define CAM_PIN_D0 5
#define CAM_PIN_VSYNC 25
#define CAM_PIN_HREF 23
#define CAM_PIN_PCLK 22

/* ========================================================
   GIAO DIỆN WEB (HTML + JavaScript)
   ======================================================== */
// Lấy điểm bắt đầu và kết thúc của file index.html đã được nhúng trong bộ nhớ
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

/* ========================================================
   CÁC HÀM XỬ LÝ CỦA WEB SERVER
   ======================================================== */

// Xử lý khi khách vào trang chủ (Trả về HTML)
static esp_err_t root_handler(httpd_req_t *req) {
// Tính toán dung lượng của file HTML
    size_t index_html_len = index_html_end - index_html_start;
    
    // Gửi ra trình duyệt
    httpd_resp_send(req, (const char *)index_html_start, index_html_len);
    return ESP_OK;
}

// Xử lý bật/tắt đèn Flash
static esp_err_t led_handler(httpd_req_t *req) {
    char buf[50];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (strstr(buf, "state=1")) {
            gpio_set_level(FLASH_LED_GPIO, 1);
            ESP_LOGI(TAG, "Da BAT den Flash");
        } else if (strstr(buf, "state=0")) {
            gpio_set_level(FLASH_LED_GPIO, 0);
            ESP_LOGI(TAG, "Da TAT den Flash");
        }
    }
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// --- HÀM PHÁT VIDEO TRỰC TIẾP ---
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    char * part_buf[64];

    // Mở "đường ống" vĩnh cửu
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK){
        return res;
    }

    // Vòng lặp tử thần: Chụp -> Gửi -> Chụp -> Gửi liên tục
// Vòng lặp tử thần: Chụp -> Gửi -> Chụp -> Gửi liên tục
    while(true){
        // 1. Bấm giờ
        int64_t start_time = esp_timer_get_time();
        
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
        }

        // 2. Lần lượt gửi các mảnh dữ liệu
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if(res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if(res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }

        // 3. Nếu gửi thành công trót lọt, ta tiến hành tính FPS (Tính lúc fb vẫn còn sống)
        if(res == ESP_OK) {
            int64_t end_time = esp_timer_get_time(); 
            int64_t frame_time_us = end_time - start_time; 
            if (frame_time_us > 0) {
                float fps = 1000000.0 / frame_time_us; 
                ESP_LOGI(TAG, "Khung hinh: %u bytes | Thoi gian: %llu ms | FPS: %.1f", fb->len, frame_time_us / 1000, fps);
            }
        }

        // 4. Dọn rác bộ nhớ NHAY LẬP TỨC sau khi dùng xong
        if(fb){
            esp_camera_fb_return(fb);
            fb = NULL;
        }

        // 5. Nếu có bất kỳ lỗi đứt mạng nào xảy ra ở các bước trên -> Thoát vòng lặp
        if(res != ESP_OK){
            break;
        }
    }
    return res;
}
/* ========================================================
   CẤU HÌNH VÀ KHỞI TẠO
   ======================================================== */

static void start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_handle_t stream_server = NULL; // Tạo thêm server thứ 2
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // 1. Khởi tạo Server 1 (Cổng 80 - Lo giao diện và Nút bấm)
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t led_uri = { .uri = "/led", .method = HTTP_GET, .handler = led_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &led_uri);
    }

    // 2. Tăng số cổng lên 81
    config.server_port += 1;
    config.ctrl_port += 1;

    // 3. Khởi tạo Server 2 (Cổng 81 - Chuyên trị Live Stream)
    if (httpd_start(&stream_server, &config) == ESP_OK) {
        httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
        httpd_register_uri_handler(stream_server, &stream_uri);
    }
}

// Hàm xử lý sự kiện Wi-Fi rút gọn
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Mat ket noi Wi-Fi, dang thu lai...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "===============================================");
        ESP_LOGI(TAG, "KET NOI THANH CONG! IP CUA CAMERA LA: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "===============================================");
        start_webserver(); // Có mạng rồi mới bật Web Server
    }
}

void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void) {
    // 1. Khởi tạo NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      nvs_flash_init();
    }

    // 2. Cấu hình chân Đèn Flash
    gpio_reset_pin(FLASH_LED_GPIO);
    gpio_set_direction(FLASH_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(FLASH_LED_GPIO, 0); // Tắt đèn ngay khi khởi động

    // 3. Khởi tạo Camera
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = CAM_PIN_D0; config.pin_d1 = CAM_PIN_D1; config.pin_d2 = CAM_PIN_D2; config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4; config.pin_d5 = CAM_PIN_D5; config.pin_d6 = CAM_PIN_D6; config.pin_d7 = CAM_PIN_D7;
    config.pin_xclk = CAM_PIN_XCLK; config.pin_pclk = CAM_PIN_PCLK; config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF; config.pin_sccb_sda = CAM_PIN_SIOD; config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn = CAM_PIN_PWDN; config.pin_reset = CAM_PIN_RESET;
    //Ép nhịp tim của Camera
    config.xclk_freq_hz = 20000000;
    config.frame_size = FRAMESIZE_VGA; // (640x480)
    // config.frame_size = FRAMESIZE_QVGA;  //(320x240)
    // config.frame_size = FRAMESIZE_QQVGA;  //(160x120)
    // config.frame_size = FRAMESIZE_UXGA; // (1600x1200)
    

    config.pixel_format = PIXFORMAT_JPEG; 
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM; 
    // Số càng to, ảnh càng xấu!
    // config.jpeg_quality = 63;
    config.jpeg_quality = 15;
    config.fb_count = 20;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Khoi tao Camera THAT BAI! Kiem tra lai PSRAM trong menuconfig!");
        return;
    }
    ESP_LOGI(TAG, "Khoi tao Camera THANH CONG!");

    // 4. Khởi tạo Wi-Fi (Sẽ tự động gọi start_webserver sau khi có IP)
    wifi_init_sta();
}