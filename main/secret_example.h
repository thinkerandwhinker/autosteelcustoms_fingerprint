/**
 * SECRET_H
 *
 * template for secret.h
 *
 * every value in secret will be validating during compile & build process
 *   MQTT host/user/pass : max host legth is 128 char, max port length is 5 char
 *   AUTH user           : 3-32 char,
 *   AUTH pass           : 8-64 char
 *   AP password         : 8-63 char
 *
 * this file will provide default value as long NVS doesn't got the value from config
 * change on CONFIG MODE only affect to NVS, this file always constant
 * deleting value on NVS will force ESP using default value from this file
 */
#ifndef SECRET_H
#define SECRET_H

/**
 * default falue for broker
 */
#define SECRET_DEFAULT_MQTT_HOST "broker.example.com"
#define SECRET_DEFAULT_MQTT_PORT "1883"
#define SECRET_DEFAULT_MQTT_USER "client-username"
#define SECRET_DEFAULT_MQTT_PASS "client-password"

/**
 * default falue for captive portal
 */
#define SECRET_DEFAULT_AUTH_USER "admin"
#define SECRET_DEFAULT_AUTH_PASS "AdminJuga123"

/**
 * default value for access point
 */
#define SECRET_AP_PASSWORD "AdminJuga123"

#endif