#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "driver/ledc.h"
#include "cJSON.h"

#define BUZZER_GPIO 25
#define WIFI_SSID "ArchTimer32"
#define WIFI_PASS "BoilerUp"

static const char *TAG = "ArchTimer32";

// --- Configuration Structure ---
typedef struct {
    int t_setup, t_shoot, t_warn;
    bool en_setup, en_shoot, en_warn;
    int f_base, f_dev, f_mod_ms;
    int duty, b_len;
} timer_config_t;

timer_config_t cfg = {
    .t_setup = 10, .t_shoot = 120, .t_warn = 30,
    .en_setup = true, .en_shoot = true, .en_warn = true,
    .f_base = 3400, .f_dev = 200, .f_mod_ms = 75,
    .duty = 128, .b_len = 600
};

// --- State Management ---
typedef enum { IDLE, SETUP, SHOOTING, STOPPED_SAFE } session_phase_t;
session_phase_t current_phase = IDLE;
int time_left = 0;
bool is_active = false;

// --- Web File References ---
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t config_html_start[] asm("_binary_config_html_start");
extern const uint8_t config_html_end[]   asm("_binary_config_html_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");

// --- Hardware Control: FM Synthesis Whistle ---
void whistle(int count) {
    for (int i = 0; i < count; i++) {
        int elapsed = 0;
        bool toggle = false;

        while (elapsed < cfg.b_len) {
            int current_f = toggle ? (cfg.f_base + cfg.f_dev) : (cfg.f_base - cfg.f_dev);
            
            // Safety check: ensure frequency is at least 100Hz to avoid div_param errors
            if (current_f < 100) current_f = 100;

            ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, current_f);
            // Scale 0-255 config to 0-1023 hardware duty
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, cfg.duty * 4);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

            vTaskDelay(pdMS_TO_TICKS(cfg.f_mod_ms));
            elapsed += cfg.f_mod_ms;
            toggle = !toggle;
        }

        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// --- Background Timer Task ---
void timer_task(void *pv) {
    while (1) {
        if (is_active && time_left > 0) {
            time_left--;
            // Warning bell logic
            if (current_phase == SHOOTING && cfg.en_warn && time_left == cfg.t_warn) {
                whistle(1); 
            }
            
            if (time_left == 0) {
                if (current_phase == SETUP) {
                    current_phase = SHOOTING;
                    time_left = cfg.t_shoot;
                    whistle(1); 
                } else {
                    is_active = false;
                    current_phase = STOPPED_SAFE;
                    whistle(3); 
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// --- HTTP Handlers ---
esp_err_t get_status_handler(httpd_req_t *req) {
    char timer_str[16];
    snprintf(timer_str, sizeof(timer_str), "%02d:%02d", time_left / 60, time_left % 60);
    const char* phase_names[] = {"READY", "SETUP", "SHOOTING", "SAFE / PULL"};
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "time", timer_str);
    cJSON_AddStringToObject(root, "phase", phase_names[current_phase]);
    cJSON_AddBoolToObject(root, "is_running", is_active);
    
    const char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free((void*)out); cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t save_config_handler(httpd_req_t *req) {
    char buf[1024];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (root) {
        #define PARSE(f) if(cJSON_GetObjectItem(root, #f)) cfg.f = cJSON_GetObjectItem(root, #f)->valueint
        #define PARSE_B(f) if(cJSON_GetObjectItem(root, #f)) cfg.f = cJSON_IsTrue(cJSON_GetObjectItem(root, #f))
        PARSE(t_setup); PARSE(t_shoot); PARSE(t_warn);
        PARSE_B(en_setup); PARSE_B(en_shoot); PARSE_B(en_warn);
        PARSE(f_base); PARSE(f_dev); PARSE(f_mod_ms);
        PARSE(duty); PARSE(b_len);
        cJSON_Delete(root);
    }
    return httpd_resp_sendstr(req, "OK");
}

esp_err_t get_config_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "t_setup", cfg.t_setup);
    cJSON_AddNumberToObject(root, "t_shoot", cfg.t_shoot);
    cJSON_AddNumberToObject(root, "t_warn", cfg.t_warn);
    cJSON_AddBoolToObject(root, "en_setup", cfg.en_setup);
    cJSON_AddBoolToObject(root, "en_shoot", cfg.en_shoot);
    cJSON_AddBoolToObject(root, "en_warn", cfg.en_warn);
    cJSON_AddNumberToObject(root, "f_base", cfg.f_base);
    cJSON_AddNumberToObject(root, "f_dev", cfg.f_dev);
    cJSON_AddNumberToObject(root, "f_mod_ms", cfg.f_mod_ms);
    cJSON_AddNumberToObject(root, "duty", cfg.duty);
    cJSON_AddNumberToObject(root, "b_len", cfg.b_len);
    const char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free((void*)out); cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t start_handler(httpd_req_t *req) {
    if (cfg.en_setup) {
        current_phase = SETUP; time_left = cfg.t_setup; whistle(2);
    } else {
        current_phase = SHOOTING; time_left = cfg.t_shoot; whistle(1);
    }
    is_active = true;
    return httpd_resp_sendstr(req, "OK");
}

esp_err_t stop_handler(httpd_req_t *req) {
    is_active = false; current_phase = IDLE; time_left = 0;
    whistle(5); 
    return httpd_resp_sendstr(req, "OK");
}

esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}
esp_err_t config_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)config_html_start, config_html_end - config_html_start);
}
esp_err_t style_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, (const char *)style_css_start, style_css_end - style_css_start);
}

// --- Initialization ---
void app_main(void) {
    nvs_flash_init();
    ESP_LOGI(TAG, "Boiler Up! ArchTimer32 Starting...");

	// PWM Hardware Setup
	ledc_timer_config_t t_cfg = {
		.speed_mode       = LEDC_LOW_SPEED_MODE,
		.timer_num        = LEDC_TIMER_0,
		.duty_resolution  = LEDC_TIMER_10_BIT, // Increased resolution for better clock division
		.freq_hz          = 2500,               // Default starting frequency
		.clk_cfg          = LEDC_AUTO_CLK       // Let the driver choose the best source (APB or XTAL)
	};
	esp_err_t err = ledc_timer_config(&t_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
	}

	ledc_channel_config_t c_cfg = {
		.gpio_num       = BUZZER_GPIO,
		.speed_mode     = LEDC_LOW_SPEED_MODE,
		.channel        = LEDC_CHANNEL_0,
		.timer_sel      = LEDC_TIMER_0,
		.duty           = 0,
		.hpoint         = 0
	};
	err = ledc_channel_config(&c_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(err));
	}
	// Networking
    esp_netif_init(); esp_event_loop_create_default(); esp_netif_create_default_wifi_ap();
    wifi_init_config_t w_cfg = WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&w_cfg);
    wifi_config_t ap_cfg = { .ap = { .ssid=WIFI_SSID, .password=WIFI_PASS, .authmode=3, .max_connection=4 } };
    esp_wifi_set_mode(WIFI_MODE_AP); esp_wifi_set_config(WIFI_IF_AP, &ap_cfg); esp_wifi_start();

    // Web Server
    httpd_handle_t server = NULL;
    httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &server_cfg) == ESP_OK) {
        httpd_uri_t r={.uri="/", .method=HTTP_GET, .handler=index_get_handler},
                    c={.uri="/config", .method=HTTP_GET, .handler=config_get_handler},
                    s={.uri="/style.css", .method=HTTP_GET, .handler=style_get_handler},
                    st={.uri="/start", .method=HTTP_GET, .handler=start_handler},
                    sp={.uri="/stop", .method=HTTP_GET, .handler=stop_handler},
                    stat={.uri="/status", .method=HTTP_GET, .handler=get_status_handler},
                    g_j={.uri="/get_config", .method=HTTP_GET, .handler=get_config_handler},
                    s_j={.uri="/save_config", .method=HTTP_POST, .handler=save_config_handler};
        
        httpd_register_uri_handler(server, &r); httpd_register_uri_handler(server, &c); httpd_register_uri_handler(server, &s);
        httpd_register_uri_handler(server, &st); httpd_register_uri_handler(server, &sp); httpd_register_uri_handler(server, &stat);
        httpd_register_uri_handler(server, &g_j); httpd_register_uri_handler(server, &s_j);
    }
    xTaskCreate(timer_task, "timer_task", 4096, NULL, 5, NULL);
}
