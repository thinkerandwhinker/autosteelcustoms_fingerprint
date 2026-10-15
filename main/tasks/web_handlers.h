/**
 * WEB_HANDLERS_H
 *
 * Routes:
 *
 * INDEX
 * - GET => / (index)
 *   redirect to /wifi if already logged in, otherwise, to /login
 *
 * AUTH PATH
 * - GET => /login
 *   request for login page
 * - POST => /login
 *   try to login
 * - POST => /logout
 *   logout
 *
 * wifi settings path:
 * - GET => /wifi
 *   request for wifi setting page
 * - POST => /wifi
 *   save wifi crendetial
 *
 * BROKER MQTT
 * - GET => /broker
 *   request for broker credential setting page
 * - POST => /broker
 *   save new broker credential (host, port, user, password)
 * - POST => /broker/reset
 *   reset broker credential depent on secret
 *
 * DEVICE
 * - GET => /device
 *   request for device credential setting page
 * - POST => /device/id (Device ID)
 *   save new device_id
 * - POST => /device/login
 *   save new captive portal credential setting (username/password)
 * - GET => /apply
 *   request for saved credentials setting
 * - POST => /apply
 *   save creadentials settings,  command ESP to restart
 *
 * all 'save' action will rewrite current value on NVS
 */
#ifndef WEB_HANDLERS_H
#define WEB_HANDLERS_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t web_handlers_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif