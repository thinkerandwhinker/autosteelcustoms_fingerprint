#include "nvs_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

#include "secret.h"
#include "sha256.h"
#include "validators.h"

#if !defined(SECRET_DEFAULT_MQTT_HOST) || !defined(SECRET_DEFAULT_MQTT_PORT) || \
    !defined(SECRET_DEFAULT_MQTT_USER) || !defined(SECRET_DEFAULT_MQTT_PASS) || \
    !defined(SECRET_DEFAULT_AUTH_USER) || !defined(SECRET_DEFAULT_AUTH_PASS) || \
    !defined(SECRET_AP_PASSWORD)
#error "secret.h belum diperbarui. Salin makro terbaru dari secret_example.h"
#endif

#define SLEN(s) (sizeof(s) - 1)
_Static_assert(SLEN(SECRET_DEFAULT_MQTT_HOST) <= MAX_LEN_MQTT_HOST, "SECRET_DEFAULT_MQTT_HOST terlalu panjang");
_Static_assert(SLEN(SECRET_DEFAULT_MQTT_PORT) <= MAX_LEN_MQTT_PORT, "SECRET_DEFAULT_MQTT_PORT terlalu panjang");
_Static_assert(SLEN(SECRET_DEFAULT_MQTT_USER) <= MAX_LEN_MQTT_USER, "SECRET_DEFAULT_MQTT_USER terlalu panjang");
_Static_assert(SLEN(SECRET_DEFAULT_MQTT_PASS) <= MAX_LEN_MQTT_PASS, "SECRET_DEFAULT_MQTT_PASS terlalu panjang");
_Static_assert(SLEN(SECRET_DEFAULT_AUTH_USER) >= 3 && SLEN(SECRET_DEFAULT_AUTH_USER) <= MAX_LEN_AUTH_USER,
               "SECRET_DEFAULT_AUTH_USER harus 3-32 karakter");
_Static_assert(SLEN(SECRET_DEFAULT_AUTH_PASS) >= 8 && SLEN(SECRET_DEFAULT_AUTH_PASS) <= MAX_LEN_AUTH_PASS,
               "SECRET_DEFAULT_AUTH_PASS harus 8-64 karakter");

static const char *TAG = "nvs_cfg";

/**
 * utils
 */
static bool open_ro(nvs_handle_t *h)
{
    return nvs_open(NVS_NAMESPACE_APP, NVS_READONLY, h) == ESP_OK;
}

/**
 * reading string, return exists=false when requested key not found
 */
static esp_err_t get_str(nvs_handle_t h, bool have, const char *key, char *buf, size_t size, bool *exists)
{
    buf[0] = '\0';
    if (exists)
        *exists = false;
    if (!have)
        return ESP_OK;

    size_t len = size;
    esp_err_t err = nvs_get_str(h, key, buf, &len);
    if (err == ESP_OK)
    {
        if (exists)
            *exists = true;
        return ESP_OK;
    }
    buf[0] = '\0';
    if (err == ESP_ERR_NVS_NOT_FOUND)
        return ESP_OK;
    ESP_LOGW(TAG, "Gagal membaca '%s': %s", key, esp_err_to_name(err));
    return err;
}

/**
 * do write for changed value only
 */
static esp_err_t set_if_changed(nvs_handle_t h, const char *key, const char *value)
{
    char cur[MAX_LEN_MQTT_HOST + 2];
    size_t len = sizeof(cur);
    if (nvs_get_str(h, key, cur, &len) == ESP_OK && strcmp(cur, value) == 0)
    {
        return ESP_OK;
    }
    return nvs_set_str(h, key, value);
}

/**
 * do write for only existing key or when new value not same with current value
 */
static esp_err_t set_with_default(nvs_handle_t h, const char *key, const char *value, const char *dflt)
{
    char cur[MAX_LEN_MQTT_HOST + 2];
    size_t len = sizeof(cur);
    esp_err_t e = nvs_get_str(h, key, cur, &len);
    if (e == ESP_ERR_NVS_NOT_FOUND)
    {
        return strcmp(value, dflt) == 0 ? ESP_OK : nvs_set_str(h, key, value);
    }
    if (e == ESP_OK && strcmp(cur, value) == 0)
        return ESP_OK;
    return nvs_set_str(h, key, value);
}

static esp_err_t erase_key(nvs_handle_t h, const char *key)
{
    esp_err_t e = nvs_erase_key(h, key);
    return (e == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : e;
}

static esp_err_t commit_close(nvs_handle_t h, esp_err_t err)
{
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++)
    {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool from_hex(const char *in, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        int hi = hex_nibble(in[i * 2]), lo = hex_nibble(in[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/**
 * comparison constant time vs buffer length
 */
static bool ct_str_equal(const char *a, const char *b, size_t cap)
{
    size_t la = strnlen(a, cap), lb = strnlen(b, cap);
    uint8_t diff = (uint8_t)(la != lb);
    for (size_t i = 0; i < cap; i++)
    {
        uint8_t ca = (i < la) ? (uint8_t)a[i] : 0;
        uint8_t cb = (i < lb) ? (uint8_t)b[i] : 0;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff == 0;
}

static void hash_password(const uint8_t salt[16], const char *pass, uint8_t out[32])
{
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, salt, 16);
    sha256_update(&c, pass, strlen(pass));
    sha256_final(&c, out);
}

/**
 * load nvs
 */
esp_err_t nvs_config_load(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t h = 0;
    const bool have = open_ro(&h);
    esp_err_t first = ESP_OK, e;
    bool ex;

#define LOAD(key, field)                                            \
    e = get_str(h, have, key, cfg->field, sizeof(cfg->field), &ex); \
    if (e != ESP_OK && first == ESP_OK)                             \
        first = e;
#define LOAD_DEF(key, field, dflt) \
    LOAD(key, field)               \
    if (!ex)                       \
        strlcpy(cfg->field, dflt, sizeof(cfg->field));

    LOAD(NVS_KEY_WIFI_SSID, wifi_ssid)
    LOAD(NVS_KEY_WIFI_PASS, wifi_pass)
    LOAD_DEF(NVS_KEY_MQTT_HOST, mqtt_host, SECRET_DEFAULT_MQTT_HOST)
    LOAD_DEF(NVS_KEY_MQTT_PORT, mqtt_port, SECRET_DEFAULT_MQTT_PORT)
    LOAD_DEF(NVS_KEY_MQTT_USER, mqtt_user, SECRET_DEFAULT_MQTT_USER)
    LOAD_DEF(NVS_KEY_MQTT_PASS, mqtt_pass, SECRET_DEFAULT_MQTT_PASS)
    LOAD(NVS_KEY_DEVICE_ID, device_id)
    LOAD_DEF(NVS_KEY_AUTH_USER, auth_user, SECRET_DEFAULT_AUTH_USER)
#undef LOAD
#undef LOAD_DEF

    if (have)
        nvs_close(h);
    return first;
}

uint32_t nvs_config_missing_mask(void)
{
    app_config_t *cfg = calloc(1, sizeof(app_config_t));
    if (cfg == NULL)
    {
        return CFG_MISSING_WIFI | CFG_MISSING_BROKER | CFG_MISSING_DEVICE_ID;
    }
    nvs_config_load(cfg);

    uint32_t mask = 0;
    char norm[MAX_LEN_DEVICE_ID + 1];
    char port[MAX_LEN_MQTT_PORT + 1];

    if (!validate_wifi_ssid(cfg->wifi_ssid))
        mask |= CFG_MISSING_WIFI;
    if (!validate_host(cfg->mqtt_host) || !validate_port(cfg->mqtt_port, port, sizeof(port)))
    {
        mask |= CFG_MISSING_BROKER;
    }
    if (!validate_device_id(cfg->device_id, norm))
        mask |= CFG_MISSING_DEVICE_ID;

    free(cfg);
    return mask;
}

/**
 * handle saving wifi credentials
 */
esp_err_t nvs_config_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_APP, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    char old_ssid[MAX_LEN_WIFI_SSID + 1], old_pass[MAX_LEN_WIFI_PASS + 1];
    get_str(h, true, NVS_KEY_WIFI_SSID, old_ssid, sizeof(old_ssid), NULL);
    get_str(h, true, NVS_KEY_WIFI_PASS, old_pass, sizeof(old_pass), NULL);

    bool changed = (strcmp(old_ssid, ssid) != 0);
    err = set_if_changed(h, NVS_KEY_WIFI_SSID, ssid);
    if (err == ESP_OK && pass != NULL)
    {
        if (strcmp(old_pass, pass) != 0)
            changed = true;
        err = set_if_changed(h, NVS_KEY_WIFI_PASS, pass);
    }
    if (err == ESP_OK && changed)
    {
        err = erase_key(h, NVS_KEY_WIFI_VERIFIED); // erase credential who not verified yet
    }
    return commit_close(h, err);
}

bool nvs_config_is_wifi_verified(void)
{
    nvs_handle_t h;
    bool have = open_ro(&h);
    char buf[2];
    bool ex = false;
    get_str(h, have, NVS_KEY_WIFI_VERIFIED, buf, sizeof(buf), &ex);
    if (have)
        nvs_close(h);
    return ex && buf[0] == '1';
}

esp_err_t nvs_config_set_wifi_verified(bool verified)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_APP, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = verified ? nvs_set_str(h, NVS_KEY_WIFI_VERIFIED, "1") : erase_key(h, NVS_KEY_WIFI_VERIFIED);
    return commit_close(h, err);
}

/**
 * handle saving for broker credential
 */
esp_err_t nvs_config_save_mqtt(const char *host, const char *port, const char *user, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_APP, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;

    err = set_with_default(h, NVS_KEY_MQTT_HOST, host, SECRET_DEFAULT_MQTT_HOST);
    if (err == ESP_OK)
        err = set_with_default(h, NVS_KEY_MQTT_PORT, port, SECRET_DEFAULT_MQTT_PORT);
    if (err == ESP_OK)
        err = set_with_default(h, NVS_KEY_MQTT_USER, user, SECRET_DEFAULT_MQTT_USER);
    if (err == ESP_OK && pass != NULL)
    {
        err = set_with_default(h, NVS_KEY_MQTT_PASS, pass, SECRET_DEFAULT_MQTT_PASS);
    }
    return commit_close(h, err);
}

bool nvs_config_mqtt_overridden(void)
{
    nvs_handle_t h;
    if (!open_ro(&h))
        return false;
    size_t len = 0;
    bool any = nvs_get_str(h, NVS_KEY_MQTT_HOST, NULL, &len) == ESP_OK ||
               nvs_get_str(h, NVS_KEY_MQTT_PORT, NULL, &len) == ESP_OK ||
               nvs_get_str(h, NVS_KEY_MQTT_USER, NULL, &len) == ESP_OK ||
               nvs_get_str(h, NVS_KEY_MQTT_PASS, NULL, &len) == ESP_OK;
    nvs_close(h);
    return any;
}

esp_err_t nvs_config_reset_mqtt(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_APP, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = erase_key(h, NVS_KEY_MQTT_HOST);
    if (err == ESP_OK)
        err = erase_key(h, NVS_KEY_MQTT_PORT);
    if (err == ESP_OK)
        err = erase_key(h, NVS_KEY_MQTT_USER);
    if (err == ESP_OK)
        err = erase_key(h, NVS_KEY_MQTT_PASS);
    return commit_close(h, err);
}

/**
 * handle saving for ESP credential
 */
esp_err_t nvs_config_save_device_id(const char *id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_APP, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = set_if_changed(h, NVS_KEY_DEVICE_ID, id);
    return commit_close(h, err);
}

esp_err_t nvs_config_save_auth(const char *user, const char *password)
{
    uint8_t salt[16], digest[32];
    char salt_hex[33], hash_hex[65];

    esp_fill_random(salt, sizeof(salt));
    hash_password(salt, password, digest);
    to_hex(salt, sizeof(salt), salt_hex);
    to_hex(digest, sizeof(digest), hash_hex);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_APP, NVS_READWRITE, &h);
    if (err == ESP_OK)
    {
        err = nvs_set_str(h, NVS_KEY_AUTH_USER, user);
        if (err == ESP_OK)
            err = nvs_set_str(h, NVS_KEY_AUTH_SALT, salt_hex);
        if (err == ESP_OK)
            err = nvs_set_str(h, NVS_KEY_AUTH_HASH, hash_hex);
        err = commit_close(h, err);
    }
    memset(digest, 0, sizeof(digest));
    return err;
}

static void read_auth(char *user, size_t usz, char *hash, size_t hsz, char *salt, size_t ssz, bool *custom)
{
    nvs_handle_t h;
    bool have = open_ro(&h);
    bool eu, eh, es;
    get_str(h, have, NVS_KEY_AUTH_USER, user, usz, &eu);
    get_str(h, have, NVS_KEY_AUTH_HASH, hash, hsz, &eh);
    get_str(h, have, NVS_KEY_AUTH_SALT, salt, ssz, &es);
    if (have)
        nvs_close(h);
    *custom = eu && eh && es && strlen(hash) == 64 && strlen(salt) == 32;
}

bool nvs_config_auth_is_default(void)
{
    char u[MAX_LEN_AUTH_USER + 1], hh[65], s[33];
    bool custom;
    read_auth(u, sizeof(u), hh, sizeof(hh), s, sizeof(s), &custom);
    return !custom;
}

esp_err_t nvs_config_reset_auth(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_APP, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = erase_key(h, NVS_KEY_AUTH_USER);
    if (err == ESP_OK)
        err = erase_key(h, NVS_KEY_AUTH_HASH);
    if (err == ESP_OK)
        err = erase_key(h, NVS_KEY_AUTH_SALT);
    return commit_close(h, err);
}

bool nvs_config_verify_login(const char *user, const char *pass)
{
    if (user == NULL)
        user = "";
    if (pass == NULL)
        pass = "";

    char su[MAX_LEN_AUTH_USER + 1], shash[65], ssalt[33];
    bool custom;
    read_auth(su, sizeof(su), shash, sizeof(shash), ssalt, sizeof(ssalt), &custom);

    bool ok = false;
    if (custom)
    {
        uint8_t salt[16], calc[32];
        char calc_hex[65];
        if (from_hex(ssalt, salt, sizeof(salt)))
        {
            hash_password(salt, pass, calc);
            to_hex(calc, sizeof(calc), calc_hex);
            bool u = ct_str_equal(user, su, MAX_LEN_AUTH_USER + 1);
            bool p = ct_str_equal(calc_hex, shash, sizeof(calc_hex));
            ok = u & p;
            memset(calc, 0, sizeof(calc));
        }
    }
    else
    {
        bool u = ct_str_equal(user, SECRET_DEFAULT_AUTH_USER, MAX_LEN_AUTH_USER + 1);
        bool p = ct_str_equal(pass, SECRET_DEFAULT_AUTH_PASS, MAX_LEN_AUTH_PASS + 1);
        ok = u & p;
    }
    memset(shash, 0, sizeof(shash));
    return ok;
}

/**
 * is force_cfg?
 */
bool nvs_config_is_force_cfg(void)
{
    nvs_handle_t h;
    bool have = open_ro(&h);
    char buf[2];
    bool ex = false;
    get_str(h, have, NVS_KEY_FORCE_CFG, buf, sizeof(buf), &ex);
    if (have)
        nvs_close(h);
    return ex && buf[0] == '1';
}

/**
 * set force_cfg
 */
esp_err_t nvs_config_set_force_cfg(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_APP, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    err = on ? nvs_set_str(h, NVS_KEY_FORCE_CFG, "1") : erase_key(h, NVS_KEY_FORCE_CFG);
    return commit_close(h, err);
}