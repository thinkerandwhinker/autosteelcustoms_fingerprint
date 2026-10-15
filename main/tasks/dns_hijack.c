#include "dns_hijack.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns_hijack";

#define DNS_PORT 53
#define DNS_MAX_PKT 512
#define DNS_ANSWER_LEN 16 // ptr(2) type(2) class(2) ttl(4) rdlen(2) rdata(4)
#define DNS_TTL_SEC 60
#define DNS_HEADER_LEN 12

static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static uint8_t s_ip[4]; // network byte order (katanya gemini sih gitu)

/**
 * offset immediately at QTYPE+QCLASS of the first question, or -1 if invalid
 */
static int question_end(const uint8_t *pkt, int len)
{
    int pos = DNS_HEADER_LEN;
    while (pos < len)
    {
        uint8_t l = pkt[pos];
        if (l == 0)
        {
            pos++;
            break;
        }
        if ((l & 0xC0) != 0)
        {
            return -1; // question will not compressed
        }
        pos += l + 1;
    }
    if (pos + 4 > len)
    {
        return -1;
    }
    return pos + 4;
}

/**
 * create responses
 * @return respons length or -1 when package was ignored
 */
static int build_response(const uint8_t *rx, int len, uint8_t *tx)
{
    if (len < DNS_HEADER_LEN)
    {
        return -1;
    }
    uint16_t flags = (uint16_t)((rx[2] << 8) | rx[3]);
    uint16_t qd = (uint16_t)((rx[4] << 8) | rx[5]);
    // only basic query (QR=0, OPCODE=0), at least 1 question
    if ((flags & 0x8000) || ((flags >> 11) & 0x0F) != 0 || qd < 1)
    {
        return -1;
    }

    int q_end = question_end(rx, len);
    if (q_end < 0)
    {
        return -1;
    }
    uint16_t qtype = (uint16_t)((rx[q_end - 4] << 8) | rx[q_end - 3]);
    bool is_a = (qtype == 1);

    memcpy(tx, rx, q_end);                    // 1st question ID
    tx[2] = (uint8_t)(0x80 | (rx[2] & 0x01)); // QR=1, RD copied
    tx[3] = 0x80;                             // RA=1, RCODE=0
    tx[4] = 0;
    tx[5] = 1; // QDCOUNT = 1
    tx[6] = 0;
    tx[7] = is_a ? 1 : 0;                // ANCOUNT
    tx[8] = tx[9] = tx[10] = tx[11] = 0; // NSCOUNT, ARCOUNT (kata gemini suruh begini)

    int o = q_end;
    if (is_a)
    {
        tx[o++] = 0xC0;
        tx[o++] = 0x0C; // pointer offset, name location is at 12th byte (kata gemini)
        tx[o++] = 0x00;
        tx[o++] = 0x01; // type A
        tx[o++] = 0x00;
        tx[o++] = 0x01; // class IN
        tx[o++] = 0x00;
        tx[o++] = 0x00;
        tx[o++] = 0x00;
        tx[o++] = DNS_TTL_SEC; // TTL
        tx[o++] = 0x00;
        tx[o++] = 0x04; // RDLENGTH
        memcpy(&tx[o], s_ip, 4);
        o += 4;
    }
    // other type will fallback into A with NOERROR. exp: AAAA => A
    return o;
}

static void dns_server_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0)
    {
        ESP_LOGE(TAG, "Gagal membuat socket (errno=%d)", errno);
        goto exit_task;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        ESP_LOGE(TAG, "Gagal bind port %d (errno=%d)", DNS_PORT, errno);
        close(sock);
        goto exit_task;
    }

    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "DNS hijack aktif di port %d", DNS_PORT);

    uint8_t rx[DNS_MAX_PKT];
    uint8_t tx[DNS_MAX_PKT + DNS_ANSWER_LEN];

    while (s_running)
    {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&client, &clen);
        if (len < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                vTaskDelay(pdMS_TO_TICKS(100)); // avoid busy looping if error persists
            }
            continue;
        }
        int rlen = build_response(rx, len, tx);
        if (rlen > 0)
        {
            sendto(sock, tx, rlen, 0, (struct sockaddr *)&client, clen);
        }
    }

    close(sock);
    ESP_LOGI(TAG, "DNS hijack berhenti");

exit_task:
    s_running = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_hijack_start(const esp_ip4_addr_t *redirect_ip)
{
    if (s_task != NULL)
    {
        return ESP_OK;
    }
    memcpy(s_ip, &redirect_ip->addr, 4); // esp_ip4_addr_t already using network byte order
    s_running = true;

    if (xTaskCreate(dns_server_task, "dns_hijack", 4096, NULL, 4, &s_task) != pdPASS)
    {
        s_running = false;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dns_hijack_stop(void)
{
    s_running = false;
}
