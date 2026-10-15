/**
 * WIFI_AP_H
 *
 * the only one module who handle access point for CONFIG MODE
 */
#ifndef WIFI_AP_H
#define WIFI_AP_H

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief turn on access point (AP) for CONFIG MODE
     *        SSID: "Presensi-Setup-XXXX" (XXXX = last 2 bytes of the AP MAC address)
     *        PASSWORD: WPA2 password is set from secret.SECRET_AP_PASSWORD
     * @param[out] ip_out AP ip address, default 192.168.4.1
     */
    esp_err_t wifi_ap_start(esp_ip4_addr_t *ip_out);

#ifdef __cplusplus
}
#endif

#endif