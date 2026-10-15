#include "web_auth.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "nvs_config.h"

static const char *TAG = "web_auth";

#define US_PER_SEC 1000000LL

typedef struct
{
    bool used;
    char token[WEB_AUTH_TOKEN_LEN + 1];
    int64_t last_us;
} session_t;

static session_t s_sessions[WEB_AUTH_MAX_SESSIONS];
static int s_fail_count = 0;
static int64_t s_lock_until_us = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void web_auth_init(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_sessions, 0, sizeof(s_sessions));
    s_fail_count = 0;
    s_lock_until_us = 0;
    portEXIT_CRITICAL(&s_lock);
}

/**
 * login lock timer remaining
 */
uint32_t web_auth_lock_remaining_s(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    int64_t until = s_lock_until_us;
    portEXIT_CRITICAL(&s_lock);
    if (now >= until)
        return 0;
    return (uint32_t)((until - now + US_PER_SEC - 1) / US_PER_SEC);
}

auth_result_t web_auth_try_login(const char *user, const char *pass)
{
    // are there any attempts remaining?
    if (web_auth_lock_remaining_s() > 0)
    {
        return AUTH_RESULT_LOCKED;
    }

    bool ok = nvs_config_verify_login(user, pass);

    auth_result_t result = ok ? AUTH_RESULT_OK : AUTH_RESULT_BAD;
    portENTER_CRITICAL(&s_lock);
    if (ok)
    {
        s_fail_count = 0;
    }
    // is attempts limit been reached?
    else if (++s_fail_count >= WEB_AUTH_MAX_FAILS)
    {
        s_fail_count = 0;
        s_lock_until_us = esp_timer_get_time() + (int64_t)WEB_AUTH_LOCK_SEC * US_PER_SEC;
        result = AUTH_RESULT_LOCKED;
    }
    portEXIT_CRITICAL(&s_lock);

    if (result == AUTH_RESULT_LOCKED)
    {
        ESP_LOGW(TAG, "Terlalu banyak login gagal. Login dikunci %d detik", WEB_AUTH_LOCK_SEC);
    }
    return result;
}

/**
 * session
 */
static bool ct_equal(const char *a, const char *b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

void web_auth_start_session(char token_out[WEB_AUTH_TOKEN_LEN + 1])
{
    static const char hexd[] = "0123456789abcdef";
    uint8_t raw[WEB_AUTH_TOKEN_LEN / 2];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); i++)
    {
        token_out[i * 2] = hexd[raw[i] >> 4];
        token_out[i * 2 + 1] = hexd[raw[i] & 0x0F];
    }
    token_out[WEB_AUTH_TOKEN_LEN] = '\0';

    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    int slot = -1;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < WEB_AUTH_MAX_SESSIONS; i++)
    {
        bool expired = s_sessions[i].used &&
                       (now - s_sessions[i].last_us) > (int64_t)WEB_AUTH_IDLE_SEC * US_PER_SEC;
        if (!s_sessions[i].used || expired)
        {
            slot = i;
            break;
        }
        if (s_sessions[i].last_us < oldest)
        {
            oldest = s_sessions[i].last_us; // take older session
            slot = i;
        }
    }
    s_sessions[slot].used = true;
    memcpy(s_sessions[slot].token, token_out, WEB_AUTH_TOKEN_LEN + 1);
    s_sessions[slot].last_us = now;
    portEXIT_CRITICAL(&s_lock);
}

/**
 * cookie handler
 */
static bool get_cookie_token(httpd_req_t *req, char out[WEB_AUTH_TOKEN_LEN + 1])
{
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0 || len > 512)
        return false;

    char *buf = malloc(len + 1);
    if (buf == NULL)
        return false;
    bool found = false;

    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, len + 1) == ESP_OK)
    {
        char *p = buf;
        // take "sid" from cookie
        while ((p = strstr(p, "sid=")) != NULL)
        {
            if (p == buf || p[-1] == ' ' || p[-1] == ';')
            { // make sure is "sid" not "*sid" or "sid*" or "*sid*"
                p += 4;
                size_t n = 0;
                while (p[n] && p[n] != ';' && n <= WEB_AUTH_TOKEN_LEN)
                    n++;
                if (n == WEB_AUTH_TOKEN_LEN)
                {
                    memcpy(out, p, n);
                    out[n] = '\0';
                    found = true;
                }
                break;
            }
            p += 4;
        }
    }
    free(buf);
    return found;
}

/**
 * check request
 */
bool web_auth_check_request(httpd_req_t *req)
{
    char token[WEB_AUTH_TOKEN_LEN + 1];
    if (!get_cookie_token(req, token))
        return false;

    int64_t now = esp_timer_get_time();
    bool valid = false;

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < WEB_AUTH_MAX_SESSIONS; i++)
    {
        if (!s_sessions[i].used)
            continue;
        if ((now - s_sessions[i].last_us) > (int64_t)WEB_AUTH_IDLE_SEC * US_PER_SEC)
        {
            s_sessions[i].used = false; // set session as expired
            continue;
        }
        if (ct_equal(token, s_sessions[i].token, WEB_AUTH_TOKEN_LEN))
        {
            s_sessions[i].last_us = now;
            valid = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return valid;
}

/**
 * end client session
 */
void web_auth_end_session(httpd_req_t *req)
{
    char token[WEB_AUTH_TOKEN_LEN + 1];
    if (!get_cookie_token(req, token))
        return;

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < WEB_AUTH_MAX_SESSIONS; i++)
    {
        if (s_sessions[i].used && ct_equal(token, s_sessions[i].token, WEB_AUTH_TOKEN_LEN))
        {
            s_sessions[i].used = false;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

/**
 * invalidate all session
 */
void web_auth_invalidate_all(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_sessions, 0, sizeof(s_sessions));
    portEXIT_CRITICAL(&s_lock);
}