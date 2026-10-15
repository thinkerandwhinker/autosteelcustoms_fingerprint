#include "validators.h"

#include <stdio.h>
#include <string.h>

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_hex(char c)   { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool has_control_chars(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7F) return true;
    }
    return false;
}

bool validate_wifi_ssid(const char *s)
{
    size_t n = strlen(s);
    return n >= 1 && n <= 32 && !has_control_chars(s);
}

bool validate_wifi_password(const char *s)
{
    size_t n = strlen(s);
    if (has_control_chars(s)) return false;
    if (n >= 8 && n <= 63) return true;
    if (n == 64) {
        for (size_t i = 0; i < n; i++) {
            if (!is_hex(s[i])) return false;
        }
        return true;
    }
    return false;
}

bool validate_host(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > 128) return false;
    if (s[0] == '.' || s[0] == '-' || s[n - 1] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(is_alpha(c) || is_digit(c) || c == '.' || c == '-' || c == '_')) return false;
    }
    return true;
}

bool validate_port(const char *s, char *out, size_t out_size)
{
    size_t n = strlen(s);
    if (n < 1 || n > 5) return false;
    unsigned v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(s[i])) return false;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    if (v < 1 || v > 65535) return false;
    snprintf(out, out_size, "%u", v);
    return true;
}

bool validate_text_optional(const char *s, size_t max_len)
{
    return strlen(s) <= max_len && !has_control_chars(s);
}

bool validate_device_id(const char *s, char *out)
{
    if (strlen(s) != 36) return false;
    for (int i = 0; i < 36; i++) {
        bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
        if (dash ? (s[i] != '-') : !is_hex(s[i])) return false;
    }
    for (int i = 0; i < 36; i++) {
        char c = s[i];
        out[i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
    }
    out[36] = '\0';
    return true;
}

bool validate_username(const char *s)
{
    size_t n = strlen(s);
    if (n < 3 || n > 32) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(is_alpha(c) || is_digit(c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

bool validate_password(const char *s)
{
    size_t n = strlen(s);
    return n >= 8 && n <= 64 && !has_control_chars(s);
}
