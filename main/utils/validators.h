/**
 * VALIDATORS_H
 *
 * validate form input value
 */
#ifndef VALIDATORS_H
#define VALIDATORS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    bool has_control_chars(const char *s);

    // SSID validation
    bool validate_wifi_ssid(const char *s);

    // wifi password validation
    bool validate_wifi_password(const char *s);

    // broker hostname validation
    bool validate_host(const char *s);

    // broker port validation
    bool validate_port(const char *s, char *out, size_t out_size);

    // text validation
    bool validate_text_optional(const char *s, size_t max_len);

    // uuid v4 validation, resulting normalize value in lowercase
    bool validate_device_id(const char *s, char *out);

    // username & password captive portal validation
    bool validate_username(const char *s);
    bool validate_password(const char *s);

#ifdef __cplusplus
}
#endif

#endif
