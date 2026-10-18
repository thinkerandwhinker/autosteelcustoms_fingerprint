/**
 * NVS_CONFIG_H => app_config
 *
 * works above NVS, all value will be string
 *
 * RULES: DELETE key = reset to default value from secret
 */
#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * 'namespace' naming rules
 *
 * max lenght of key is 15 char
 */
#define NVS_NAMESPACE_APP "app_config"

#define NVS_KEY_WIFI_SSID "wifi_ssid"
#define NVS_KEY_WIFI_PASS "wifi_pass"
#define NVS_KEY_WIFI_VERIFIED "wifi_verified" // "1" = verified
#define NVS_KEY_MQTT_HOST "mqtt_host"
#define NVS_KEY_MQTT_PORT "mqtt_port"
#define NVS_KEY_MQTT_USER "mqtt_user"
#define NVS_KEY_MQTT_PASS "mqtt_pass"
#define NVS_KEY_DEVICE_ID "device_id"
#define NVS_KEY_AUTH_USER "auth_user"
#define NVS_KEY_AUTH_HASH "auth_hash" // hex SHA-256 (salt || password)
#define NVS_KEY_AUTH_SALT "auth_salt" // random 16 bytes hex
#define NVS_KEY_FORCE_CFG "force_cfg" // "1" will set next boot into CONFIG MODE
#define NVS_KEY_LAST_REQ "last_req"   // last handled request_id from backend

/**
 * max length in byte withour null terminatot
 */
#define MAX_LEN_WIFI_SSID 32
#define MAX_LEN_WIFI_PASS 64
#define MAX_LEN_MQTT_HOST 128
#define MAX_LEN_MQTT_PORT 5
#define MAX_LEN_MQTT_USER 128
#define MAX_LEN_MQTT_PASS 128
#define MAX_LEN_DEVICE_ID 36
#define MAX_LEN_AUTH_USER 32
#define MAX_LEN_AUTH_PASS 64
#define MAX_LEN_REQUEST_ID 64

/**
 * mark for uncompleted configuration
 */
#define CFG_MISSING_WIFI (1u << 0)
#define CFG_MISSING_BROKER (1u << 1)
#define CFG_MISSING_DEVICE_ID (1u << 2)

    typedef struct
    {
        char wifi_ssid[MAX_LEN_WIFI_SSID + 1];
        char wifi_pass[MAX_LEN_WIFI_PASS + 1];
        char mqtt_host[MAX_LEN_MQTT_HOST + 1];
        char mqtt_port[MAX_LEN_MQTT_PORT + 1];
        char mqtt_user[MAX_LEN_MQTT_USER + 1];
        char mqtt_pass[MAX_LEN_MQTT_PASS + 1];
        char device_id[MAX_LEN_DEVICE_ID + 1];
        char auth_user[MAX_LEN_AUTH_USER + 1];
    } app_config_t;

    /**
     * load nvs
     *
     * if namespace does not exist yet is not an error
     */
    esp_err_t nvs_config_load(app_config_t *cfg);

    uint32_t nvs_config_missing_mask(void);

    /**
     * wifi manipulation start from this line
     */

    /**
     * @param pass
     *
     * NULL = holded
     * "" = open network
     * else = new password
     *
     * verified badge wil remove when value changed
     */
    esp_err_t nvs_config_save_wifi(const char *ssid, const char *pass);
    bool nvs_config_is_wifi_verified(void);
    esp_err_t nvs_config_set_wifi_verified(bool verified);

    /**
     * broker manipulation start from this line
     */

    /**
     * @param pass
     *
     * NULL = holded
     * "" = open network
     * else = new password
     *
     * same value as default will not rewrite
     */
    esp_err_t nvs_config_save_mqtt(const char *host, const char *port, const char *user, const char *pass);
    bool nvs_config_mqtt_overridden(void);
    esp_err_t nvs_config_reset_mqtt(void);

    /**
     * esp credential manipulation start from this line
     */
    esp_err_t nvs_config_save_device_id(const char *id);

    /**
     * caotive protal configuration start from this line
     */

    /**
     * save new username & (hash) password
     */
    esp_err_t nvs_config_save_auth(const char *user, const char *password);

    /**
     * will true if no custom credential on NVS
     */
    bool nvs_config_auth_is_default(void);

    /**
     * reset custom credential and set to default credential
     */
    esp_err_t nvs_config_reset_auth(void);

    /**
     * verify login in constant time
     */
    bool nvs_config_verify_login(const char *user, const char *pass);

    /**
     * to triggering configuration mode
     */
    bool nvs_config_is_force_cfg(void);
    esp_err_t nvs_config_set_force_cfg(bool on);

    /**
     * managing last handled reques_id from backend
     */
    esp_err_t nvs_config_get_last_request(char *buf, size_t size);
    esp_err_t nvs_config_set_last_request(const char *request_id);

#ifdef __cplusplus
}
#endif

#endif