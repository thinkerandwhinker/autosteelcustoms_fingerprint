/**
 * STATE_MANAGER_H
 *
 * ESP global state, including RTOS & event group
 * this module deciding
 * State global perangkat (FreeRTOS Event Group) dan penentuan kelengkapan
 * this module decide whether the device should enter operational mode or configuration mode
 */
#ifndef STATE_MANAGER_H
#define STATE_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C"
{
#endif

    extern EventGroupHandle_t sys_state_group;

#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT1
#define MODE_CONFIG_BIT BIT2
#define DEVICE_BUSY_BIT BIT3

    /**
     * NVS flash initialization
     * will erase the configuration if the NVS is full
     * or overwrite it if a new version is available
     */
    esp_err_t state_manager_nvs_init(void);

    /**
     * idemp global event group
     */
    esp_err_t state_manager_init(void);

    /**
     * @brief is configuration data complete?
     *
     * true when wifi_ssid, mqtt_host, mqtt_port, and device_id has filled in
     * just validates the completeness required for booting into operational mode
     *
     * connection success is now handled by each individual module
     * which utilizes configuration data: WiFi & MQTT
     */
    bool check_credentials_exist(void);

#ifdef __cplusplus
}
#endif

#endif