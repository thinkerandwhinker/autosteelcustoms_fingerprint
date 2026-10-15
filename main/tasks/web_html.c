#include "web_html.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_config.h"

static const char *TAG = "web_html";

#define MAX_BODY_LEN 2048

#define HTML_HEAD_1                                                   \
    "<!DOCTYPE html><html lang=\"id\"><head><meta charset=\"utf-8\">" \
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>"

#define HTML_HEAD_2                                                                                                  \
    "</title><style>"                                                                                                \
    ":root{--bg:#f2f4f7;--card:#fff;--fg:#1d2733;--mut:#5b6775;--bd:#c9d1da;--pri:#1565c0;"                          \
    "--ok:#1b7f3b;--warn:#9a5b00;--err:#b00020;--okbg:#e6f4ea;--warnbg:#fff4e0;--errbg:#fde8ec}"                     \
    "@media(prefers-color-scheme:dark){:root{--bg:#12161b;--card:#1c2229;--fg:#e6ebf0;--mut:#9aa7b5;"                \
    "--bd:#38424d;--pri:#5aa2ff;--ok:#5fd18a;--warn:#f0b04a;--err:#ff7a8a;--okbg:#17321f;"                           \
    "--warnbg:#3a2c10;--errbg:#3d1a21}}"                                                                             \
    "body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--fg);margin:0;padding:12px}"              \
    ".wrap{max-width:480px;margin:0 auto}"                                                                           \
    "nav{display:flex;flex-wrap:wrap;gap:6px;align-items:center;margin-bottom:12px}"                                 \
    "nav a,nav button{padding:7px 11px;border-radius:6px;background:var(--card);color:var(--fg);"                    \
    "border:1px solid var(--bd);text-decoration:none;font-size:.85rem;width:auto;margin:0}"                          \
    "nav a.on{background:var(--pri);color:#fff;border-color:var(--pri)}"                                             \
    "nav form{margin:0 0 0 auto}"                                                                                    \
    ".badge{font-size:.75rem;padding:3px 8px;border-radius:999px}"                                                   \
    ".badge.ok{background:var(--okbg);color:var(--ok)}.badge.warn{background:var(--warnbg);color:var(--warn)}"       \
    ".card{background:var(--card);padding:18px;border-radius:10px;border:1px solid var(--bd);margin-bottom:12px}"    \
    "h1{font-size:1.15rem;margin:0 0 4px}.mut{color:var(--mut);font-size:.85rem;margin:0 0 6px}"                     \
    "label{display:block;margin-top:12px;font-size:.9rem;font-weight:600}"                                           \
    "label.chk{display:flex;gap:8px;align-items:center;font-weight:400}"                                             \
    "input{width:100%;box-sizing:border-box;padding:9px;margin-top:4px;border:1px solid var(--bd);"                  \
    "border-radius:6px;font-size:1rem;background:var(--bg);color:var(--fg)}"                                         \
    "input[type=checkbox]{width:auto;margin:0}"                                                                      \
    "small{display:block;color:var(--mut);font-size:.78rem;margin-top:3px}"                                          \
    "button{width:100%;margin-top:18px;padding:11px;background:var(--pri);color:#fff;border:0;"                      \
    "border-radius:6px;font-size:1rem}button.sec{background:var(--card);color:var(--fg);border:1px solid var(--bd)}" \
    "button:disabled{opacity:.5}"                                                                                    \
    ".msg{padding:10px 12px;border-radius:6px;margin-bottom:12px;font-size:.9rem}"                                   \
    ".msg.ok{background:var(--okbg);color:var(--ok)}.msg.err{background:var(--errbg);color:var(--err)}"              \
    ".msg.warn{background:var(--warnbg);color:var(--warn)}"                                                          \
    "table{width:100%;border-collapse:collapse;margin-top:8px;font-size:.9rem}"                                      \
    "td{padding:7px 4px;border-bottom:1px solid var(--bd);word-break:break-all}"                                     \
    "td.ok{color:var(--ok)}td.warn{color:var(--warn)}td.miss{color:var(--err)}"                                      \
    "</style></head><body><div class=\"wrap\">"

void web_html_security_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; "
                       "frame-ancestors 'none'; base-uri 'none'");
}

void web_html_escape(char *dst, size_t dst_size, const char *src)
{
    size_t o = 0;
    for (; *src; src++)
    {
        const char *rep = NULL;
        switch (*src)
        {
        case '&':
            rep = "&amp;";
            break;
        case '<':
            rep = "&lt;";
            break;
        case '>':
            rep = "&gt;";
            break;
        case '"':
            rep = "&quot;";
            break;
        case '\'':
            rep = "&#39;";
            break;
        default:
            break;
        }
        if (rep)
        {
            size_t n = strlen(rep);
            if (o + n + 1 > dst_size)
                break;
            memcpy(dst + o, rep, n);
            o += n;
        }
        else
        {
            if (o + 2 > dst_size)
                break;
            dst[o++] = *src;
        }
    }
    dst[o] = '\0';
}

esp_err_t web_html_send(httpd_req_t *req, const char *s)
{
    return httpd_resp_send_chunk(req, s, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_html_sendf(httpd_req_t *req, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t send_head(httpd_req_t *req, const char *title)
{
    web_html_security_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    esp_err_t ret = web_html_send(req, HTML_HEAD_1);
    if (ret == ESP_OK)
        ret = web_html_send(req, title);
    if (ret == ESP_OK)
        ret = web_html_send(req, HTML_HEAD_2);
    return ret;
}

static esp_err_t send_flash(httpd_req_t *req, const char *flash, bool is_err)
{
    if (flash == NULL)
        return ESP_OK;
    return web_html_sendf(req, "<div class=\"msg %s\">%s</div>", is_err ? "err" : "ok", flash);
}

esp_err_t web_html_begin_plain(httpd_req_t *req, const char *title, const char *flash, bool flash_is_error)
{
    esp_err_t ret = send_head(req, title);
    if (ret == ESP_OK)
        ret = send_flash(req, flash, flash_is_error);
    return ret;
}

esp_err_t web_html_begin(httpd_req_t *req, const char *title, nav_item_t active,
                         const char *flash, bool flash_is_error)
{
    esp_err_t ret = send_head(req, title);
    if (ret != ESP_OK)
        return ret;

    const bool complete = (nvs_config_missing_mask() == 0);
    ret = web_html_sendf(req,
                         "<nav>"
                         "<a href=\"/wifi\"%s>WiFi</a><a href=\"/broker\"%s>Broker</a>"
                         "<a href=\"/device\"%s>Perangkat</a><a href=\"/apply\"%s>Terapkan &amp; Restart</a>"
                         "<span class=\"badge %s\">%s</span>"
                         "<form method=\"POST\" action=\"/logout\"><button type=\"submit\">Keluar</button></form>"
                         "</nav>",
                         active == NAV_WIFI ? " class=\"on\"" : "",
                         active == NAV_BROKER ? " class=\"on\"" : "",
                         active == NAV_DEVICE ? " class=\"on\"" : "",
                         active == NAV_APPLY ? " class=\"on\"" : "",
                         complete ? "ok" : "warn", complete ? "Lengkap" : "Belum lengkap");
    if (ret == ESP_OK)
        ret = send_flash(req, flash, flash_is_error);
    return ret;
}

esp_err_t web_html_end(httpd_req_t *req)
{
    esp_err_t ret = web_html_send(req, "</div></body></html>");
    if (ret == ESP_OK)
        ret = httpd_resp_send_chunk(req, NULL, 0);
    return ret;
}

esp_err_t web_html_field(httpd_req_t *req, const char *label, const char *name, const char *type,
                         const char *value, size_t maxlen, bool required,
                         const char *extra, const char *hint)
{
    // max char length 128
    char esc[6 * 128 + 1];
    char buf[1280];

    web_html_escape(esc, sizeof(esc), value ? value : "");

    int n = snprintf(buf, sizeof(buf),
                     "<label for=\"%s\">%s</label>"
                     "<input id=\"%s\" name=\"%s\" type=\"%s\" maxlength=\"%u\" value=\"%s\" "
                     "autocapitalize=\"off\" spellcheck=\"false\"%s%s>",
                     name, label, name, name, type, (unsigned)maxlen, esc,
                     required ? " required" : "", extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof(buf))
        return ESP_FAIL;

    if (hint)
    {
        int m = snprintf(buf + n, sizeof(buf) - n, "<small>%s</small>", hint);
        if (m < 0 || (size_t)(n + m) >= sizeof(buf))
            return ESP_FAIL;
        n += m;
    }
    return httpd_resp_send_chunk(req, buf, n);
}

esp_err_t web_html_checkbox(httpd_req_t *req, const char *name, const char *label, bool checked)
{
    return web_html_sendf(req,
                          "<label class=\"chk\"><input type=\"checkbox\" name=\"%s\" value=\"1\"%s> %s</label>",
                          name, checked ? " checked" : "", label);
}

esp_err_t web_redirect(httpd_req_t *req, const char *location, const char *set_cookie, bool close_conn)
{
    web_html_security_headers(req);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    if (set_cookie)
        httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);
    if (close_conn)
        httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

/**
 * form component
 */
esp_err_t web_form_read(httpd_req_t *req, web_form_t *f)
{
    memset(f, 0, sizeof(*f));
    size_t total = req->content_len;
    if (total == 0 || total > MAX_BODY_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    f->body = malloc(total + 1);
    f->tmp = malloc(total + 1);
    if (f->body == NULL || f->tmp == NULL)
    {
        web_form_free(f);
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;
    while (got < total)
    {
        int r = httpd_req_recv(req, f->body + got, total - got);
        if (r <= 0)
        {
            web_form_free(f);
            return (r == HTTPD_SOCK_ERR_TIMEOUT) ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        got += (size_t)r;
    }
    f->body[total] = '\0';
    f->len = total;
    return ESP_OK;
}

void web_form_free(web_form_t *f)
{
    free(f->body);
    free(f->tmp);
    memset(f, 0, sizeof(*f));
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

/**
 * url decoding
 */
static bool url_decode(char *s)
{
    char *o = s;
    while (*s)
    {
        if (*s == '+')
        {
            *o++ = ' ';
            s++;
        }
        else if (*s == '%')
        {
            if (!isxdigit((unsigned char)s[1]) || !isxdigit((unsigned char)s[2]))
                return false;
            *o++ = (char)((hex_val(s[1]) << 4) | hex_val(s[2]));
            s += 3;
        }
        else
        {
            *o++ = *s++;
        }
    }
    *o = '\0';
    return true;
}

static void trim_inplace(char *s)
{
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
    size_t lead = 0;
    while (s[lead] && isspace((unsigned char)s[lead]))
        lead++;
    if (lead)
        memmove(s, s + lead, strlen(s + lead) + 1);
}

form_result_t web_form_get(const web_form_t *f, const char *key, char *out, size_t out_size, bool trim)
{
    out[0] = '\0';
    if (f->body == NULL)
        return FORM_OK;

    esp_err_t e = httpd_query_key_value(f->body, key, f->tmp, f->len + 1);
    if (e == ESP_ERR_NOT_FOUND)
        return FORM_OK;
    if (e != ESP_OK)
        return FORM_BAD;

    if (!url_decode(f->tmp))
        return FORM_BAD;
    if (trim)
        trim_inplace(f->tmp);

    for (const char *p = f->tmp; *p; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F)
            return FORM_BAD;
    }
    size_t n = strlen(f->tmp);
    if (n >= out_size)
        return FORM_TOO_LONG;

    memcpy(out, f->tmp, n + 1);
    return FORM_OK;
}

bool web_form_has(const web_form_t *f, const char *key)
{
    if (f->body == NULL)
        return false;
    esp_err_t e = httpd_query_key_value(f->body, key, f->tmp, f->len + 1);
    if (e != ESP_OK && e != ESP_ERR_HTTPD_RESULT_TRUNC)
    {
        ESP_LOGD(TAG, "key '%s' tidak ada", key);
        return false;
    }
    return true;
}