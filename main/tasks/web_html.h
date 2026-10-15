/**
 * WEB_HTML_C
 *
 * providing layout components
 */
#ifndef WEB_HTML_H
#define WEB_HTML_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        NAV_NONE = 0,
        NAV_WIFI,
        NAV_BROKER,
        NAV_DEVICE,
        NAV_APPLY,
    } nav_item_t;

    /**
     * page with navbar
     */
    esp_err_t web_html_begin(httpd_req_t *req, const char *title, nav_item_t active,
                             const char *flash, bool flash_is_error);
    /**
     * page without navbar
     */
    esp_err_t web_html_begin_plain(httpd_req_t *req, const char *title,
                                   const char *flash, bool flash_is_error);
    esp_err_t web_html_end(httpd_req_t *req);

    esp_err_t web_html_send(httpd_req_t *req, const char *s);
    esp_err_t web_html_sendf(httpd_req_t *req, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    /**
     * input component
     */
    esp_err_t web_html_field(httpd_req_t *req, const char *label, const char *name, const char *type,
                             const char *value, size_t maxlen, bool required,
                             const char *extra, const char *hint);
    esp_err_t web_html_checkbox(httpd_req_t *req, const char *name, const char *label, bool checked);

    /**
     * escaping html char, like &quote; whatever...
     */
    void web_html_escape(char *dst, size_t dst_size, const char *src);

    void web_html_security_headers(httpd_req_t *req);

    esp_err_t web_redirect(httpd_req_t *req, const char *location, const char *set_cookie, bool close_conn);

    /**
     * form
     * (application/x-www-form-urlencoded)
     */
    typedef struct
    {
        char *body;
        char *tmp;
        size_t len;
    } web_form_t;

    typedef enum
    {
        FORM_OK = 0,
        FORM_TOO_LONG,
        FORM_BAD, // when input including forbidden char
    } form_result_t;

    /**
     * read post body, max size 2048 byte
     */
    esp_err_t web_form_read(httpd_req_t *req, web_form_t *f);
    void web_form_free(web_form_t *f);

    form_result_t web_form_get(const web_form_t *f, const char *key, char *out, size_t out_size, bool trim);

    /**
     * for checkbox check
     */
    bool web_form_has(const web_form_t *f, const char *key);

#ifdef __cplusplus
}
#endif

#endif