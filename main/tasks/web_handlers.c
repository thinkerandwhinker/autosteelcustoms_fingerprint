#include "web_handlers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "nvs_config.h"
#include "ui_task.h"
#include "validators.h"
#include "web_auth.h"
#include "web_html.h"
#include "web_server.h"

static const char *TAG = "web_pages";

#define ST_BAD_REQUEST "400 Bad Request"
#define ST_UNAUTH "401 Unauthorized"
#define ST_TOO_MANY "429 Too Many Requests"
#define ST_SERVER_ERR "500 Internal Server Error"

#define TRY(expr)          \
    do                     \
    {                      \
        ret = (expr);      \
        if (ret != ESP_OK) \
            goto out;      \
    } while (0)
#define SEND(s) TRY(web_html_send(req, (s)))

static const struct
{
    const char *code;
    const char *text;
    bool err;
} FLASH_TABLE[] = {
    {"wifi_ok", "Pengaturan WiFi tersimpan.", false},
    {"broker_ok", "Pengaturan broker tersimpan.", false},
    {"broker_reset", "Broker dikembalikan ke nilai default firmware.", false},
    {"id_ok", "Device ID tersimpan.", false},
    {"login_ok", "Login diperbarui. Silakan masuk kembali.", false},
    {"bye", "Anda telah keluar.", false},
    {"expired", "Sesi berakhir. Silakan masuk kembali.", true},
};

static const char *flash_from_query(httpd_req_t *req, bool *is_err)
{
    *is_err = false;
    size_t ql = httpd_req_get_url_query_len(req);
    if (ql == 0 || ql > 64)
        return NULL;

    char q[66], code[24];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK)
        return NULL;
    if (httpd_query_key_value(q, "m", code, sizeof(code)) != ESP_OK)
        return NULL;

    for (size_t i = 0; i < sizeof(FLASH_TABLE) / sizeof(FLASH_TABLE[0]); i++)
    {
        if (strcmp(code, FLASH_TABLE[i].code) == 0)
        {
            *is_err = FLASH_TABLE[i].err;
            return FLASH_TABLE[i].text;
        }
    }
    return NULL;
}

static bool require_auth(httpd_req_t *req)
{
    web_server_touch_activity();
    if (web_auth_check_request(req))
    {
        return true;
    }
    web_redirect(req, "/login", NULL, req->method == HTTP_POST);
    return false;
}

static esp_err_t bad_form(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Permintaan tidak valid");
}

static esp_err_t oom(httpd_req_t *req)
{
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memori tidak cukup");
}

/**
 * login
 */
static esp_err_t render_login(httpd_req_t *req, const char *status, const char *flash,
                              bool is_err, const char *user_show)
{
    esp_err_t ret;
    if (status)
        httpd_resp_set_status(req, status);

    TRY(web_html_begin_plain(req, "Login", flash, is_err));
    SEND("<div class=\"card\"><h1>Login Konfigurasi</h1>"
         "<p class=\"mut\">Masuk untuk mengatur perangkat presensi.</p>"
         "<form method=\"POST\" action=\"/login\" accept-charset=\"utf-8\">");
    TRY(web_html_field(req, "Username", "username", "text", user_show ? user_show : "",
                       MAX_LEN_AUTH_USER, true, " autocomplete=\"username\"", NULL));
    TRY(web_html_field(req, "Password", "password", "password", "",
                       MAX_LEN_AUTH_PASS, true, " autocomplete=\"current-password\"", NULL));
    SEND("<button type=\"submit\">Masuk</button></form></div>");
    TRY(web_html_end(req));
out:
    return ret;
}

static esp_err_t root_get(httpd_req_t *req)
{
    web_server_touch_activity();
    return web_redirect(req, web_auth_check_request(req) ? "/wifi" : "/login", NULL, false);
}

static esp_err_t login_get(httpd_req_t *req)
{
    web_server_touch_activity();
    if (web_auth_check_request(req))
    {
        return web_redirect(req, "/wifi", NULL, false);
    }
    bool is_err;
    const char *flash = flash_from_query(req, &is_err);
    return render_login(req, NULL, flash, is_err, NULL);
}

static esp_err_t login_post(httpd_req_t *req)
{
    web_server_touch_activity();

    web_form_t f;
    if (web_form_read(req, &f) != ESP_OK)
        return bad_form(req);

    char user[MAX_LEN_AUTH_USER + 1] = {0};
    char pass[MAX_LEN_AUTH_PASS + 1] = {0};
    /* Input tidak valid/terlalu panjang tetap diproses sebagai percobaan gagal (dihitung). */
    if (web_form_get(&f, "username", user, sizeof(user), true) != FORM_OK)
        user[0] = '\0';
    if (web_form_get(&f, "password", pass, sizeof(pass), false) != FORM_OK)
        pass[0] = '\0';
    web_form_free(&f);

    esp_err_t ret;
    switch (web_auth_try_login(user, pass))
    {
    case AUTH_RESULT_OK:
    {
        char token[WEB_AUTH_TOKEN_LEN + 1];
        char cookie[96];
        web_auth_start_session(token);
        snprintf(cookie, sizeof(cookie), "sid=%s; Path=/; HttpOnly; SameSite=Strict", token);
        ESP_LOGI(TAG, "Login berhasil");
        ret = web_redirect(req, "/wifi", cookie, false);
        break;
    }
    case AUTH_RESULT_LOCKED:
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "Terlalu banyak percobaan. Coba lagi dalam %u detik.",
                 (unsigned)web_auth_lock_remaining_s());
        ret = render_login(req, ST_TOO_MANY, msg, true, user);
        break;
    }
    default:
        ret = render_login(req, ST_UNAUTH, "Username atau password salah.", true, user);
        break;
    }

    memset(pass, 0, sizeof(pass));
    return ret;
}

/**
 * logout
 */
static esp_err_t logout_post(httpd_req_t *req)
{
    web_server_touch_activity();
    web_auth_end_session(req);
    return web_redirect(req, "/login?m=bye",
                        "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict", true);
}

/**
 * wifi
 */
static esp_err_t render_wifi(httpd_req_t *req, const char *status, const char *flash,
                             bool is_err, const char *ssid_override)
{
    esp_err_t ret;
    app_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL)
        return oom(req);
    nvs_config_load(cfg);

    const char *ssid = ssid_override ? ssid_override : cfg->wifi_ssid;
    const bool pass_set = cfg->wifi_pass[0] != '\0';
    const bool open_net = (cfg->wifi_ssid[0] != '\0') && !pass_set;

    if (status)
        httpd_resp_set_status(req, status);

    TRY(web_html_begin(req, "WiFi", NAV_WIFI, flash, is_err));
    SEND("<div class=\"card\"><h1>Pengaturan WiFi</h1>"
         "<p class=\"mut\">Jaringan yang dipakai perangkat saat mode operasional.</p>"
         "<form method=\"POST\" action=\"/wifi\" accept-charset=\"utf-8\">");
    TRY(web_html_field(req, "SSID", "ssid", "text", ssid, MAX_LEN_WIFI_SSID, true, NULL, NULL));
    TRY(web_html_field(req, "Password WiFi", "wifi_pass", "password", "", MAX_LEN_WIFI_PASS, false,
                       " autocomplete=\"new-password\"",
                       pass_set ? "Password tersimpan. Kosongkan untuk tidak mengubah (hanya jika SSID tidak diganti)."
                                : "8-63 karakter, atau 64 digit hex."));
    TRY(web_html_checkbox(req, "open", "Jaringan tanpa password (terbuka)", open_net));
    SEND("<button type=\"submit\">Simpan WiFi</button></form></div>");
    TRY(web_html_end(req));
out:
    free(cfg);
    return ret;
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    if (!require_auth(req))
        return ESP_OK;
    bool is_err;
    const char *flash = flash_from_query(req, &is_err);
    return render_wifi(req, NULL, flash, is_err, NULL);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    if (!require_auth(req))
        return ESP_OK;

    web_form_t f;
    if (web_form_read(req, &f) != ESP_OK)
        return bad_form(req);

    app_config_t *cur = calloc(1, sizeof(*cur));
    if (cur == NULL)
    {
        web_form_free(&f);
        return oom(req);
    }
    nvs_config_load(cur);

    char ssid[MAX_LEN_WIFI_SSID + 1] = {0};
    char pass[MAX_LEN_WIFI_PASS + 1] = {0};
    const char *msg = NULL;
    const char *status = ST_BAD_REQUEST;
    const char *pass_param = NULL;
    bool open;
    esp_err_t ret;

    if (web_form_get(&f, "ssid", ssid, sizeof(ssid), false) != FORM_OK ||
        web_form_get(&f, "wifi_pass", pass, sizeof(pass), false) != FORM_OK)
    {
        msg = "SSID/password terlalu panjang atau mengandung karakter tidak valid.";
        goto invalid;
    }
    open = web_form_has(&f, "open");

    if (!validate_wifi_ssid(ssid))
    {
        msg = "SSID wajib diisi (1-32 karakter).";
        goto invalid;
    }

    // is password included?
    if (pass[0] != '\0')
    { // is password invalid?
        if (!validate_wifi_password(pass))
        {
            msg = "Password WiFi harus 8-63 karakter (atau 64 digit hex).";
            goto invalid;
        }
        pass_param = pass;
    }
    else if (open)
    {
        pass_param = ""; // password not send to client again
    }
    else if (cur->wifi_pass[0] != '\0' && strcmp(ssid, cur->wifi_ssid) == 0)
    {
        pass_param = NULL;
    }
    else
    {
        msg = "Isi password WiFi, atau centang \"Jaringan tanpa password\".";
        goto invalid;
    }

    // is saved to NVS?
    if (nvs_config_save_wifi(ssid, pass_param) != ESP_OK)
    {
        msg = "Gagal menyimpan ke penyimpanan perangkat.";
        status = ST_SERVER_ERR;
        goto invalid;
    }
    ESP_LOGI(TAG, "WiFi disimpan (SSID='%s')", ssid);
    ret = web_redirect(req, "/wifi?m=wifi_ok", NULL, false);
    goto cleanup;

invalid:
    ret = render_wifi(req, status, msg, true, ssid[0] ? ssid : NULL);
cleanup:
    memset(pass, 0, sizeof(pass));
    web_form_free(&f);
    free(cur);
    return ret;
}

/**
 * broker
 */
static esp_err_t render_broker(httpd_req_t *req, const char *status, const char *flash, bool is_err,
                               const char *host_o, const char *port_o, const char *user_o)
{
    esp_err_t ret;
    app_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL)
        return oom(req);
    nvs_config_load(cfg);
    const bool overridden = nvs_config_mqtt_overridden();
    const bool pass_set = cfg->mqtt_pass[0] != '\0';

    if (status)
        httpd_resp_set_status(req, status);

    TRY(web_html_begin(req, "Broker", NAV_BROKER, flash, is_err));
    TRY(web_html_sendf(req,
                       "<div class=\"card\"><h1>Pengaturan Broker MQTT</h1><p class=\"mut\">%s</p>"
                       "<form method=\"POST\" action=\"/broker\" accept-charset=\"utf-8\">",
                       overridden ? "Nilai telah disesuaikan." : "Memakai nilai default firmware. Ubah hanya jika perlu."));
    TRY(web_html_field(req, "Host / Broker", "host", "text", host_o ? host_o : cfg->mqtt_host,
                       MAX_LEN_MQTT_HOST, true, NULL, "Tanpa mqtt:// dan tanpa port."));
    TRY(web_html_field(req, "Port", "port", "number", port_o ? port_o : cfg->mqtt_port,
                       MAX_LEN_MQTT_PORT, true, " min=\"1\" max=\"65535\" inputmode=\"numeric\"", NULL));
    TRY(web_html_field(req, "Username (opsional)", "mqtt_user", "text", user_o ? user_o : cfg->mqtt_user,
                       MAX_LEN_MQTT_USER, false, " autocomplete=\"off\"", NULL));
    TRY(web_html_field(req, "Password (opsional)", "mqtt_pass", "password", "", MAX_LEN_MQTT_PASS, false,
                       " autocomplete=\"new-password\"",
                       pass_set ? "Password tersimpan. Kosongkan untuk tidak mengubah."
                                : "Belum ada password."));
    if (pass_set)
    {
        TRY(web_html_checkbox(req, "clear_pass", "Hapus password (tanpa password)", false));
    }
    SEND("<button type=\"submit\">Simpan Broker</button></form></div>");

    if (overridden)
    {
        SEND("<div class=\"card\"><form method=\"POST\" action=\"/broker/reset\">"
             "<p class=\"mut\">Kembalikan host, port, username, dan password broker ke nilai default firmware.</p>"
             "<button class=\"sec\" type=\"submit\">Kembalikan ke default</button></form></div>");
    }
    TRY(web_html_end(req));
out:
    free(cfg);
    return ret;
}

static esp_err_t broker_get(httpd_req_t *req)
{
    if (!require_auth(req))
        return ESP_OK;
    bool is_err;
    const char *flash = flash_from_query(req, &is_err);
    return render_broker(req, NULL, flash, is_err, NULL, NULL, NULL);
}

static esp_err_t broker_post(httpd_req_t *req)
{
    if (!require_auth(req))
        return ESP_OK;

    web_form_t f;
    if (web_form_read(req, &f) != ESP_OK)
        return bad_form(req);

    char host[MAX_LEN_MQTT_HOST + 1] = {0}, port[MAX_LEN_MQTT_PORT + 1] = {0};
    char port_norm[MAX_LEN_MQTT_PORT + 1] = {0};
    char user[MAX_LEN_MQTT_USER + 1] = {0}, pass[MAX_LEN_MQTT_PASS + 1] = {0};
    const char *msg = NULL;
    const char *status = ST_BAD_REQUEST;
    const char *pass_param = NULL;
    bool clear;
    esp_err_t ret;

    if (web_form_get(&f, "host", host, sizeof(host), true) != FORM_OK ||
        web_form_get(&f, "port", port, sizeof(port), true) != FORM_OK ||
        web_form_get(&f, "mqtt_user", user, sizeof(user), true) != FORM_OK ||
        web_form_get(&f, "mqtt_pass", pass, sizeof(pass), false) != FORM_OK)
    {
        msg = "Ada input yang terlalu panjang atau mengandung karakter tidak valid.";
        goto invalid;
    }
    clear = web_form_has(&f, "clear_pass");

    if (!validate_host(host))
    {
        msg = "Host tidak valid (contoh: broker.example.com atau 192.168.1.10, tanpa mqtt:// dan port).";
        goto invalid;
    }
    if (!validate_port(port, port_norm, sizeof(port_norm)))
    {
        msg = "Port harus berupa angka 1-65535.";
        goto invalid;
    }

    // is password included?
    if (pass[0] != '\0')
        pass_param = pass;
    else if (clear)
        pass_param = ""; // password not sent to client again
    else
        pass_param = NULL;

    // is saved?
    if (nvs_config_save_mqtt(host, port_norm, user, pass_param) != ESP_OK)
    {
        msg = "Gagal menyimpan ke penyimpanan perangkat.";
        status = ST_SERVER_ERR;
        goto invalid;
    }
    ESP_LOGI(TAG, "Broker disimpan (%s:%s)", host, port_norm);
    ret = web_redirect(req, "/broker?m=broker_ok", NULL, false);
    goto cleanup;

invalid:
    ret = render_broker(req, status, msg, true, host[0] ? host : NULL, port[0] ? port : NULL, user);
cleanup:
    memset(pass, 0, sizeof(pass));
    web_form_free(&f);
    return ret;
}

static esp_err_t broker_reset_post(httpd_req_t *req)
{
    if (!require_auth(req))
        return ESP_OK;
    if (nvs_config_reset_mqtt() != ESP_OK)
    {
        return render_broker(req, ST_SERVER_ERR, "Gagal mengembalikan ke default.", true, NULL, NULL, NULL);
    }
    ESP_LOGI(TAG, "Broker dikembalikan ke default");
    return web_redirect(req, "/broker?m=broker_reset", NULL, false);
}

/**
 * ESP credentials
 *
 * username & password captive portal
 * device_id (uuid v4 from backend), manual input from user
 */
static esp_err_t render_device(httpd_req_t *req, const char *status, const char *flash, bool is_err,
                               const char *id_override, const char *user_override)
{
    esp_err_t ret;
    app_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL)
        return oom(req);
    nvs_config_load(cfg);
    const bool auth_default = nvs_config_auth_is_default();

    if (status)
        httpd_resp_set_status(req, status);

    TRY(web_html_begin(req, "Perangkat", NAV_DEVICE, flash, is_err));

    // device_id
    SEND("<div class=\"card\"><h1>Device ID</h1>"
         "<p class=\"mut\">UUID dari backend. Wajib diisi agar perangkat dapat masuk mode operasional.</p>"
         "<form method=\"POST\" action=\"/device/id\" accept-charset=\"utf-8\">");
    TRY(web_html_field(req, "Device ID", "device_id", "text", id_override ? id_override : cfg->device_id,
                       MAX_LEN_DEVICE_ID, true,
                       " placeholder=\"123e4567-e89b-12d3-a456-426614174000\"", NULL));
    SEND("<button type=\"submit\">Simpan Device ID</button></form></div>");

    // captive portal credentials
    SEND("<div class=\"card\"><h1>Login Konfigurasi</h1>"
         "<p class=\"mut\">Username dan password untuk membuka halaman ini.</p>");
    if (auth_default)
    {
        SEND("<div class=\"msg warn\">Perangkat masih memakai login bawaan firmware. Segera ganti.</div>");
    }
    SEND("<form method=\"POST\" action=\"/device/login\" accept-charset=\"utf-8\">");
    TRY(web_html_field(req, "Username", "auth_user", "text", user_override ? user_override : cfg->auth_user,
                       MAX_LEN_AUTH_USER, true, " autocomplete=\"username\"",
                       "3-32 karakter: huruf, angka, titik, garis bawah, strip."));
    TRY(web_html_field(req, "Password saat ini", "cur_pass", "password", "", MAX_LEN_AUTH_PASS, true,
                       " autocomplete=\"current-password\"", NULL));
    TRY(web_html_field(req, "Password baru", "new_pass", "password", "", MAX_LEN_AUTH_PASS, false,
                       " autocomplete=\"new-password\"",
                       "8-64 karakter. Kosongkan jika hanya mengganti username."));
    TRY(web_html_field(req, "Ulangi password baru", "new_pass2", "password", "", MAX_LEN_AUTH_PASS, false,
                       " autocomplete=\"new-password\"", NULL));
    SEND("<button type=\"submit\">Simpan Login</button></form></div>");
    TRY(web_html_end(req));
out:
    free(cfg);
    return ret;
}

static esp_err_t device_get(httpd_req_t *req)
{
    if (!require_auth(req))
        return ESP_OK;
    bool is_err;
    const char *flash = flash_from_query(req, &is_err);
    return render_device(req, NULL, flash, is_err, NULL, NULL);
}

static esp_err_t device_id_post(httpd_req_t *req)
{
    if (!require_auth(req))
        return ESP_OK;

    web_form_t f;
    if (web_form_read(req, &f) != ESP_OK)
        return bad_form(req);

    char id[MAX_LEN_DEVICE_ID + 1] = {0};
    char norm[MAX_LEN_DEVICE_ID + 1] = {0};
    const char *msg = NULL;
    const char *status = ST_BAD_REQUEST;
    esp_err_t ret;

    if (web_form_get(&f, "device_id", id, sizeof(id), true) != FORM_OK || !validate_device_id(id, norm))
    {
        msg = "Format Device ID harus UUID, contoh: 123e4567-e89b-12d3-a456-426614174000.";
        goto invalid;
    }
    if (nvs_config_save_device_id(norm) != ESP_OK)
    {
        msg = "Gagal menyimpan ke penyimpanan perangkat.";
        status = ST_SERVER_ERR;
        goto invalid;
    }
    ESP_LOGI(TAG, "Device ID disimpan");
    ret = web_redirect(req, "/device?m=id_ok", NULL, false);
    goto cleanup;

invalid:
    ret = render_device(req, status, msg, true, id[0] ? id : NULL, NULL);
cleanup:
    web_form_free(&f);
    return ret;
}

static esp_err_t device_login_post(httpd_req_t *req)
{
    if (!require_auth(req))
        return ESP_OK;

    web_form_t f;
    if (web_form_read(req, &f) != ESP_OK)
        return bad_form(req);

    app_config_t *cur = calloc(1, sizeof(*cur));
    if (cur == NULL)
    {
        web_form_free(&f);
        return oom(req);
    }
    nvs_config_load(cur);

    char user[MAX_LEN_AUTH_USER + 1] = {0};
    char cur_pass[MAX_LEN_AUTH_PASS + 1] = {0};
    char new_pass[MAX_LEN_AUTH_PASS + 1] = {0};
    char new_pass2[MAX_LEN_AUTH_PASS + 1] = {0};
    char msgbuf[96];
    const char *msg = NULL;
    const char *status = ST_BAD_REQUEST;
    const char *final_pass;
    esp_err_t ret;

    if (web_form_get(&f, "auth_user", user, sizeof(user), true) != FORM_OK ||
        web_form_get(&f, "cur_pass", cur_pass, sizeof(cur_pass), false) != FORM_OK ||
        web_form_get(&f, "new_pass", new_pass, sizeof(new_pass), false) != FORM_OK ||
        web_form_get(&f, "new_pass2", new_pass2, sizeof(new_pass2), false) != FORM_OK)
    {
        msg = "Ada input yang terlalu panjang atau mengandung karakter tidak valid.";
        goto invalid;
    }

    if (!validate_username(user))
    {
        msg = "Username harus 3-32 karakter: huruf, angka, titik, garis bawah, atau strip.";
        goto invalid;
    }

    // verify existing password
    switch (web_auth_try_login(cur->auth_user, cur_pass))
    {
    case AUTH_RESULT_OK:
        break;
    case AUTH_RESULT_LOCKED:
        snprintf(msgbuf, sizeof(msgbuf), "Terlalu banyak percobaan. Coba lagi dalam %u detik.",
                 (unsigned)web_auth_lock_remaining_s());
        msg = msgbuf;
        status = ST_TOO_MANY;
        goto invalid;
    default:
        msg = "Password saat ini salah.";
        status = ST_UNAUTH;
        goto invalid;
    }

    // is password included?
    if (new_pass[0] != '\0')
    {
        if (!validate_password(new_pass))
        {
            msg = "Password baru harus 8-64 karakter.";
            goto invalid;
        }
        if (strcmp(new_pass, new_pass2) != 0)
        {
            msg = "Konfirmasi password baru tidak sama.";
            goto invalid;
        }
        final_pass = new_pass;
    }
    else
    {
        final_pass = cur_pass; // password on nvs with current existing password
    }

    // is dirty?
    if (strcmp(user, cur->auth_user) == 0 && strcmp(final_pass, cur_pass) == 0)
    {
        msg = "Tidak ada perubahan.";
        goto invalid;
    }

    // is saved?
    if (nvs_config_save_auth(user, final_pass) != ESP_OK)
    {
        msg = "Gagal menyimpan ke penyimpanan perangkat.";
        status = ST_SERVER_ERR;
        goto invalid;
    }

    ESP_LOGI(TAG, "Login konfigurasi diperbarui; semua sesi dibatalkan");
    web_auth_invalidate_all();
    ret = web_redirect(req, "/login?m=login_ok",
                       "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict", false);
    goto cleanup;

invalid:
    ret = render_device(req, status, msg, true, NULL, user[0] ? user : NULL);
cleanup:
    memset(cur_pass, 0, sizeof(cur_pass));
    memset(new_pass, 0, sizeof(new_pass));
    memset(new_pass2, 0, sizeof(new_pass2));
    web_form_free(&f);
    free(cur);
    return ret;
}

/**
 * save & restart
 */
static esp_err_t row(httpd_req_t *req, const char *label, const char *value, int state)
{
    static const char *const cls[] = {"ok", "warn", "miss"};
    static const char *const txt[] = {"OK", "Perhatian", "Belum diisi"};
    char esc[256];
    web_html_escape(esc, sizeof(esc), value);
    return web_html_sendf(req, "<tr><td>%s</td><td>%s</td><td class=\"%s\">%s</td></tr>",
                          label, esc, cls[state], txt[state]);
}

static esp_err_t render_apply(httpd_req_t *req, const char *status, const char *flash, bool is_err)
{
    esp_err_t ret;
    app_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL)
        return oom(req);
    nvs_config_load(cfg);
    const uint32_t missing = nvs_config_missing_mask();
    char hostport[MAX_LEN_MQTT_HOST + MAX_LEN_MQTT_PORT + 2];
    snprintf(hostport, sizeof(hostport), "%s:%s", cfg->mqtt_host, cfg->mqtt_port);

    if (status)
        httpd_resp_set_status(req, status);

    TRY(web_html_begin(req, "Terapkan", NAV_APPLY, flash, is_err));
    SEND("<div class=\"card\"><h1>Terapkan &amp; Restart</h1>"
         "<p class=\"mut\">Perangkat restart lalu masuk mode operasional memakai pengaturan di bawah.</p><table>");
    TRY(row(req, "WiFi", (missing & CFG_MISSING_WIFI) ? "-" : cfg->wifi_ssid, (missing & CFG_MISSING_WIFI) ? 2 : 0));
    TRY(row(req, "Broker", (missing & CFG_MISSING_BROKER) ? "-" : hostport, (missing & CFG_MISSING_BROKER) ? 2 : 0));
    TRY(row(req, "Device ID", (missing & CFG_MISSING_DEVICE_ID) ? "-" : cfg->device_id,
            (missing & CFG_MISSING_DEVICE_ID) ? 2 : 0));
    TRY(row(req, "Login", nvs_config_auth_is_default() ? "bawaan firmware" : cfg->auth_user,
            nvs_config_auth_is_default() ? 1 : 0));
    SEND("</table>");

    if (missing == 0)
    {
        SEND("<form method=\"POST\" action=\"/apply\"><button type=\"submit\">Terapkan &amp; Restart</button></form>");
    }
    else
    {
        SEND("<button type=\"button\" disabled>Terapkan &amp; Restart</button>"
             "<small>Lengkapi item yang belum diisi terlebih dahulu.</small>");
    }
    SEND("</div>");
    TRY(web_html_end(req));
out:
    free(cfg);
    return ret;
}

static esp_err_t apply_get(httpd_req_t *req)
{
    if (!require_auth(req))
        return ESP_OK;
    return render_apply(req, NULL, NULL, false);
}

static esp_err_t apply_post(httpd_req_t *req)
{
    if (!require_auth(req))
        return ESP_OK;

    if (nvs_config_missing_mask() != 0)
    {
        return render_apply(req, ST_BAD_REQUEST, "Konfigurasi belum lengkap.", true);
    }

    esp_err_t ret;
    TRY(web_html_begin_plain(req, "Diterapkan", NULL, false));
    SEND("<div class=\"card\"><h1>Konfigurasi diterapkan</h1>"
         "<p>Perangkat akan restart dalam beberapa detik lalu terhubung ke WiFi. "
         "Jaringan setup ini akan hilang.</p></div>");
    TRY(web_html_end(req));
out:
    ESP_LOGI(TAG, "Konfigurasi diterapkan; restart dijadwalkan");
    ui_feedback(UI_FEEDBACK_SUCCESS, "Config diterapkan", "Restart...");
    web_server_schedule_restart(2000);
    return ret;
}

/**
 * routes
 */
esp_err_t web_handlers_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/login", .method = HTTP_GET, .handler = login_get},
        {.uri = "/login", .method = HTTP_POST, .handler = login_post},
        {.uri = "/logout", .method = HTTP_POST, .handler = logout_post},
        {.uri = "/wifi", .method = HTTP_GET, .handler = wifi_get},
        {.uri = "/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/broker", .method = HTTP_GET, .handler = broker_get},
        {.uri = "/broker", .method = HTTP_POST, .handler = broker_post},
        {.uri = "/broker/reset", .method = HTTP_POST, .handler = broker_reset_post},
        {.uri = "/device", .method = HTTP_GET, .handler = device_get},
        {.uri = "/device/id", .method = HTTP_POST, .handler = device_id_post},
        {.uri = "/device/login", .method = HTTP_POST, .handler = device_login_post},
        {.uri = "/apply", .method = HTTP_GET, .handler = apply_get},   // current credential on nvs
        {.uri = "/apply", .method = HTTP_POST, .handler = apply_post}, // set credential on nvs
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
    {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Gagal mendaftarkan %s: %s", routes[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}