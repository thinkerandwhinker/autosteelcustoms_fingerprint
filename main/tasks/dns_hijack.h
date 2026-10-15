/**
 * DNS_HIJACK_H
 *
 * do intercept all type A query (www.example.com) and redirect client into ESP ip addr
 * forcing client device to open captive portal
 */

#ifndef DNS_HIJACK_H
#define DNS_HIJACK_H

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief running DNS (UDP/53) task who answer all type A query
     * with ESP ip addr to trigger captive portal on client devices
     */
    esp_err_t dns_hijack_start(const esp_ip4_addr_t *redirect_ip);

    /**
     * @brief to stop DNS task after done in <= 1 secs
     */
    void dns_hijack_stop(void);

#ifdef __cplusplus
}
#endif

#endif
