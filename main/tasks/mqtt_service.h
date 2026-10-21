/**
 * MQTT_SERVICE_H
 *
 * the one and only module who handle all mqtt comunication protocols (subscribe, unsubscribe, publish)
 *
 * client_id using saved device_id on NVS
 *
 * ESP do SUBSCRIBE into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                    qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/commands     1       listening for command from backend
 * devices/{device_id}/{channel}    1       listening for next action after
 *                                          command was received
 *
 * ESP do PUBLISH into topics:
 * ----------------------------------------------------------------------------
 * topic pattern                    qos     usage
 * ----------------------------------------------------------------------------
 * devices/{device_id}/{channel}    1       send result action while, task
 *                                          update, and common execution
 *                                          commands
 */

#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"
#include "mqtt_command.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef void (*mqtt_command_handler_t)(const mqtt_command_t *cmd, void *ctx);
    typedef void (*mqtt_message_handler_t)(const char *payload, size_t len, void *ctx);

    /**
     * do initialization and validating device_id from NVS
     */
    esp_err_t mqtt_service_init(void);

    /**
     * subscribe into additional channels
     * run only service start it's self
     */
    esp_err_t mqtt_service_subscribe(const char *channel, int qos, mqtt_message_handler_t handler, void *ctx);

    /**
     * registering command handlers and ignoring unknown commands
     * run only service start it's self
     */
    esp_err_t mqtt_service_register_command(const char *name, mqtt_command_handler_t handler, void *ctx);

    /**
     * initialize the mqtt client and the mqtt task
     * connection starts automatically once WiFi is connected
     */
    esp_err_t mqtt_service_start(void);

    /**
     * check if mqtt is connected
     * true when mqtt connected
     */
    bool mqtt_service_is_connected(void);

    /**
     * count for successfully reconnect attempt automatically
     */
    uint32_t mqtt_service_connect_count(void);

    /**
     * take device_id from NVS
     */
    const char *mqtt_service_device_id(void);

    /**
     * publish func
     * automatically clear message when got rejected by state (ESP_ERR_INVALID_STATE)
     * let backend retry his command
     */
    esp_err_t mqtt_service_publish(const char *channel, const char *payload, int qos);

    /**
     * same with publish func, but message is parse into buffer (JSON)
     *
     * sama aja sih sama mqtt_service_publish, bedanya ini rasa stroberi
     */
    esp_err_t mqtt_service_publish_json(const char *channel, const cJSON *obj, int qos);

    /**
     * this func will confirm that message received by server
     * if server send respons back into ESP
     *
     * will wait until waiting timeout reached
     * and mark process as ESP_ERR_TIMEOUT
     */
    esp_err_t mqtt_service_publish_confirmed(const char *channel, const cJSON *obj, uint32_t timeout_ms);

    /**
     * new object event
     * pattern => { "event": event, "requestId": requestId }
     */
    cJSON *mqtt_event_new(const char *event, const char *request_id);

    /**
     * stoping mqtt task gracefully
     * flow: stop mqtt client => closing connection
     *
     * CALL THIS IF NO MESSAGE WILL SEND TO BACKEND!!!
     * example: pas mo restart
     *
     * IMPLEMENTATION REASON
     * Kode kemarin ada kendala penolakan dari broker setiap ESP 32 habis restart, hampir kayak rebutan koneksi.
     * Padahal, tiap nyala ESP pasti pake clean session dan tiap alat di set pake id yang beda, tapi ntah bug library atau memang boardnya,
     * Intinya clean sessionnya gagal dibuat. Di ESP WROOM 32-U normal, tapi di ESP 32 S3 masalahnya terus terusan muncul.
     * Sampe sini, kepalaku rasanya mau jetor.
     *
     * Belajar dari nest js, biasanya ada OnModuleDestroy yang bisa buat buat graceful shutdown servis atau mutus komunikasi
     * sebelum sistem bener-bener dimatiin. Jadi, sekarang tiap mau restart by code, harus pake ini. Ga berlaku kalo modelnya
     * cabut colok listrik.
     */
    esp_err_t mqtt_service_stop(void);

#ifdef __cplusplus
}
#endif

#endif