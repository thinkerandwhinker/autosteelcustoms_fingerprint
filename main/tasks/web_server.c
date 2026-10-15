#include "web_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_config.h"
#include "ui_task.h"
#include "web_handlers.h"
#include "web_html.h"

static const char *TAG = "web_server";

#define IDLE_CHECK_PERIOD_US (30ULL * 1000ULL * 1000ULL)

static httpd_handle_t s_server = NULL;
static char s_redirect_url[32];
static volatile uint32_t s_last_activity_ms = 0;
static bool s_restart_pending = false;
static esp_timer_handle_t s_restart_timer = NULL;
static esp_timer_handle_t s_idle_timer = NULL;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void web_server_touch_activity(void)
{
    s_last_activity_ms = now_ms();
}

/**
 * scheduled restart
 */
static void restart_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Restart");
    esp_restart();
}

void web_server_schedule_restart(uint32_t delay_ms)
{
    if (s_restart_pending)
    {
        return;
    }
    s_restart_pending = true;

    const esp_timer_create_args_t args = {
        .callback = restart_timer_cb,
        .name = "cfg_restart",
    };
    if (esp_timer_create(&args, &s_restart_timer) == ESP_OK &&
        esp_timer_start_once(s_restart_timer, (uint64_t)delay_ms * 1000ULL) == ESP_OK)
    {
        ESP_LOGI(TAG, "Restart dijadwalkan dalam %u ms", (unsigned)delay_ms);
        return;
    }

    ESP_LOGW(TAG, "Timer gagal dibuat; restart dengan delay langsung");
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

/**
 * idle timer
 */
static void idle_timer_cb(void *arg)
{
    (void)arg;
    if (s_restart_pending)
    {
        return;
    }
    if ((uint32_t)(now_ms() - s_last_activity_ms) < WEB_SERVER_IDLE_TIMEOUT_MS)
    {
        return;
    }
    // is config doesn't complete?
    if (nvs_config_missing_mask() != 0)
    {
        return;
    }

    ESP_LOGW(TAG, "Tidak ada aktivitas %u menit dan konfigurasi lengkap -> kembali ke mode operasional",
             (unsigned)(WEB_SERVER_IDLE_TIMEOUT_MS / 60000u));
    ui_feedback(UI_FEEDBACK_SUCCESS, "Config timeout", "Restart...");
    web_server_schedule_restart(1500);
}

/**
 * captive portal
 */
static esp_err_t not_found_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", s_redirect_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Redirecting to configuration portal", HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_server_start(const esp_ip4_addr_t *ap_ip)
{
    if (s_server != NULL)
    {
        return ESP_OK;
    }
    snprintf(s_redirect_url, sizeof(s_redirect_url), "http://" IPSTR "/", IP2STR(ap_ip));
    web_server_touch_activity();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 4; // create more socket for beloving dns hijack, uwwu >///<
    config.max_uri_handlers = 20;
    config.max_resp_headers = 12;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start gagal: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    err = web_handlers_register(s_server);
    if (err != ESP_OK)
    {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, not_found_redirect);

    const esp_timer_create_args_t targs = {
        .callback = idle_timer_cb,
        .name = "cfg_idle",
    };
    if (esp_timer_create(&targs, &s_idle_timer) == ESP_OK)
    {
        esp_timer_start_periodic(s_idle_timer, IDLE_CHECK_PERIOD_US);
    }
    else
    {
        ESP_LOGW(TAG, "Timer idle tidak aktif");
    }

    ESP_LOGI(TAG, "Web server aktif: %s", s_redirect_url);
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_idle_timer != NULL)
    {
        esp_timer_stop(s_idle_timer);
        esp_timer_delete(s_idle_timer);
        s_idle_timer = NULL;
    }
    if (s_server != NULL)
    {
        httpd_stop(s_server);
        s_server = NULL;
    }
}