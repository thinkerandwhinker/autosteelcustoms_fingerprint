/**
 * WEB_AUTH_H
 *
 * simple authencitcation for client to get access into configuration page:
 *  - login username/password, verified by nvs_config_verify_login with constant limit time
 *  - session to create 32 random char (128 biy) as token in cookie HttpOnly; SameSite=Strict, stored in memory (hilang kalau mati mendadak ESP mati)
 *  - max handle 3 sessions at same, each session will expired in 10 minutes when no request
 *  - attempt limit & global block (anti brute force kecil-kecilan hehe): set login attempts and lock access to login, all client affected
 */
#ifndef WEB_AUTH_H
#define WEB_AUTH_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define WEB_AUTH_TOKEN_LEN 32
#define WEB_AUTH_MAX_SESSIONS 3
#define WEB_AUTH_IDLE_SEC (10 * 60)
#define WEB_AUTH_MAX_FAILS 5
#define WEB_AUTH_LOCK_SEC 60

    typedef enum
    {
        AUTH_RESULT_OK = 0,
        AUTH_RESULT_BAD,
        AUTH_RESULT_LOCKED,
    } auth_result_t;

    void web_auth_init(void);

    /**
     * credendial verification & attempts increment
     */
    auth_result_t web_auth_try_login(const char *user, const char *pass);

    /**
     * lock time remaining until attempts reset
     */
    uint32_t web_auth_lock_remaining_s(void);

    /**
     * create new session, clear old sessions if session limit are full
     */
    void web_auth_start_session(char token_out[WEB_AUTH_TOKEN_LEN + 1]);

    /**
     * true if request carry a valid session cookie & refresh session
     */
    bool web_auth_check_request(httpd_req_t *req);

    /**
     * end session/logout
     */
    void web_auth_end_session(httpd_req_t *req);

    /**
     * clear all sessions if reset captive protal credential are triggered
     * idemp, modular
     */
    void web_auth_invalidate_all(void);

#ifdef __cplusplus
}
#endif

#endif