#include "as608.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "as608";

#define HDR_H 0xEF
#define HDR_L 0x01
#define PID_COMMAND 0x01
#define PID_ACK 0x07

#define HEAD_AFTER_MAGIC 7 // address(4) + pid(1) + length(2)
#define CMD_PAYLOAD_MAX 8
#define CMD_FRAME_MAX (2 + HEAD_AFTER_MAGIC + CMD_PAYLOAD_MAX + 2)
#define ACK_BODY_MAX 48 // confirm(1) + data(max 32 from index table) + checksum(2), with some room

// instructions
#define CMD_GEN_IMG 0x01
#define CMD_IMG2TZ 0x02
#define CMD_SEARCH 0x04
#define CMD_REG_MODEL 0x05
#define CMD_STORE 0x06
#define CMD_DELETE_CHAR 0x0C
#define CMD_EMPTY 0x0D
#define CMD_READ_SYS_PARA 0x0F
#define CMD_VFY_PWD 0x13
#define CMD_TEMPLATE_NUM 0x1D
#define CMD_READ_INDEX_TABLE 0x1F

#define CHAR_BUFFER_1 1
#define CHAR_BUFFER_2 2

#define INDEX_PAGE_BITS 256 // one ReadIndexTable page = 256 slots = 32 bytes

#define ACK_TIMEOUT_MS 2000
#define SEARCH_TIMEOUT_MS 3000
#define TX_DONE_TIMEOUT_MS 100
#define POLL_INTERVAL_MS 50
#define REMOVE_CONFIRM_COUNT 2 // consecutive "no finger" readings before sensor is considered free
#define RX_BUF_SIZE 256        // must be bigger than UART HW FIFO (128)
#define VFY_RETRIES 3
#define VFY_RETRY_DELAY_MS 200

typedef enum
{
    ENROLL_IDLE = 0,
    ENROLL_FIRST_CAPTURED, // buffer 1 valid, finger still on sensor
    ENROLL_FIRST_REMOVED,  // buffer 1 valid, sensor is free
    ENROLL_MODEL_READY,    // merged, ready to save
} enroll_state_t;

static struct
{
    as608_config_t cfg;
    as608_info_t info;
    SemaphoreHandle_t lock;
    enroll_state_t enroll;
    uint8_t last_code;
} s;

static volatile bool s_ready = false;
static volatile bool s_cancel = false;

/**
 * helpers start from this line
 */
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static inline uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

static uint16_t checksum16(const uint8_t *p, size_t n)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < n; i++)
        sum = (uint16_t)(sum + p[i]);
    return sum;
}

// how long since start?
static inline TickType_t elapsed_since(TickType_t start)
{
    return (TickType_t)(xTaskGetTickCount() - start);
}

const char *as608_status_str(as608_status_t status)
{
    switch (status)
    {
    case AS608_OK:
        return "OK";
    case AS608_ERR_INVALID_ARG:
        return "INVALID_ARG";
    case AS608_ERR_BAD_STATE:
        return "BAD_STATE";
    case AS608_ERR_INTERNAL:
        return "INTERNAL";
    case AS608_ERR_NO_RESPONSE:
        return "NO_RESPONSE";
    case AS608_ERR_COMM:
        return "COMM";
    case AS608_ERR_TIMEOUT:
        return "TIMEOUT";
    case AS608_ERR_CANCELLED:
        return "CANCELLED";
    case AS608_ERR_NO_FINGER:
        return "NO_FINGER";
    case AS608_ERR_IMAGE_FAIL:
        return "IMAGE_FAIL";
    case AS608_ERR_POOR_IMAGE:
        return "POOR_IMAGE";
    case AS608_ERR_MISMATCH:
        return "MISMATCH";
    case AS608_ERR_DUPLICATE:
        return "DUPLICATE";
    case AS608_ERR_NOT_FOUND:
        return "NOT_FOUND";
    case AS608_ERR_BAD_ID:
        return "BAD_ID";
    case AS608_ERR_SLOT_OCCUPIED:
        return "SLOT_OCCUPIED";
    case AS608_ERR_FLASH:
        return "FLASH";
    case AS608_ERR_ERASE_FAILED:
        return "ERASE_FAILED";
    case AS608_ERR_WRONG_PASSWORD:
        return "WRONG_PASSWORD";
    case AS608_ERR_SENSOR:
        return "SENSOR";
    }
    return "UNKNOWN";
}

uint8_t as608_last_confirm_code(void) { return s.last_code; }
bool as608_is_ready(void) { return s_ready; }
void as608_cancel(void) { s_cancel = true; }

/**
 * sensor confirm code => status
 * only the codes that matter to the caller get their own status
 * for unknowing reason i don't even learn, throw as AS608_ERR_SENSOR
 */
static as608_status_t map_confirm(uint8_t code)
{
    switch (code)
    {
    case 0x00:
        return AS608_OK;
    case 0x01:
        return AS608_ERR_COMM; // sensor got a broken packet from us
    case 0x02:
        return AS608_ERR_NO_FINGER;
    case 0x03:
        return AS608_ERR_IMAGE_FAIL;
    case 0x06: // if burik
    case 0x07: // too noisy
        return AS608_ERR_POOR_IMAGE;
    case 0x08: // not same finger at first scan
    case 0x0A: // sama
        return AS608_ERR_MISMATCH;
    case 0x09:
        return AS608_ERR_NOT_FOUND;
    case 0x0B:
        return AS608_ERR_BAD_ID;
    case 0x13:
        return AS608_ERR_WRONG_PASSWORD;
    case 0x18:
        return AS608_ERR_FLASH;
    case 0x10: // delete failed
    case 0x11: // empty action failed
        return AS608_ERR_ERASE_FAILED;
    default:
        return AS608_ERR_SENSOR;
    }
}

/**
 * flow: send command, receive ack
 */
static as608_status_t send_command(const uint8_t *payload, size_t len)
{
    if (len == 0 || len > CMD_PAYLOAD_MAX)
        return AS608_ERR_INVALID_ARG;

    uint8_t frame[CMD_FRAME_MAX];
    size_t n = 0;
    frame[n++] = HDR_H;
    frame[n++] = HDR_L;
    put32(&frame[n], s.cfg.address);
    n += 4;
    frame[n++] = PID_COMMAND;
    put16(&frame[n], (uint16_t)(len + 2));
    n += 2;
    memcpy(&frame[n], payload, len);
    n += len;
    put16(&frame[n], checksum16(&frame[6], n - 6)); // from pid to the end of payload
    n += 2;

    // drop whatever is still in RX
    uart_flush_input(s.cfg.uart_num);

    if (uart_write_bytes(s.cfg.uart_num, frame, n) != (int)n)
        return AS608_ERR_COMM;
    uart_wait_tx_done(s.cfg.uart_num, pdMS_TO_TICKS(TX_DONE_TIMEOUT_MS));
    return AS608_OK;
}

static bool read_exact(uint8_t *buf, size_t n, TickType_t start, TickType_t total)
{
    size_t got = 0;
    while (got < n)
    {
        const TickType_t el = elapsed_since(start);
        if (el >= total)
            return false;
        const int r = uart_read_bytes(s.cfg.uart_num, buf + got, (uint32_t)(n - got), total - el);
        if (r < 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

/**
 * @param body receives confirm code + data (checksum is verified then dropped)
 * @param body_len body len
 */
static as608_status_t recv_ack(uint8_t *body, size_t body_cap, size_t *body_len, uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t total = pdMS_TO_TICKS(timeout_ms);

    bool seen_hdr_h = false;

    // hunt for 0xEF 0x01
    for (;;)
    {
        uint8_t b;
        if (!read_exact(&b, 1, start, total))
            return AS608_ERR_NO_RESPONSE;
        if (b == HDR_H)
            seen_hdr_h = true;
        else if (seen_hdr_h && b == HDR_L)
            break;
        else
            seen_hdr_h = false;
    }

    uint8_t head[HEAD_AFTER_MAGIC];
    if (!read_exact(head, sizeof(head), start, total))
        return AS608_ERR_COMM;

    const uint32_t addr = get32(&head[0]);
    const uint8_t pid = head[4];
    const uint16_t flen = get16(&head[5]);
    if (addr != s.cfg.address || pid != PID_ACK || flen < 3 || flen > body_cap)
    {
        ESP_LOGW(TAG, "Header balasan tidak valid (addr=0x%08lx pid=0x%02x len=%u)", (unsigned long)addr, pid, flen);
        return AS608_ERR_COMM;
    }

    if (!read_exact(body, flen, start, total))
        return AS608_ERR_COMM;

    const uint16_t calc = (uint16_t)(checksum16(&head[4], 3) + checksum16(body, (size_t)flen - 2));
    if (calc != get16(&body[flen - 2]))
    {
        ESP_LOGW(TAG, "Checksum balasan salah");
        return AS608_ERR_COMM;
    }
    *body_len = (size_t)flen - 2;
    return AS608_OK;
}

/**
 * one command + its ack
 * @param data receives the first expect bytes after confirm code, may be NULL when expect == 0
 * @param expect how many data bytes must be in the reply
 */
static as608_status_t transact(const uint8_t *cmd, size_t cmd_len, uint8_t *data, size_t expect, uint32_t timeout_ms)
{
    uint8_t body[ACK_BODY_MAX];
    size_t blen = 0;

    as608_status_t st = send_command(cmd, cmd_len);
    if (st != AS608_OK)
        return st;
    st = recv_ack(body, sizeof(body), &blen, timeout_ms);
    if (st != AS608_OK)
        return st;

    s.last_code = body[0];
    st = map_confirm(body[0]);
    if (st != AS608_OK)
        return st;

    if (blen - 1 < expect)
        return AS608_ERR_COMM; // shorter reply
    if (expect > 0)
        memcpy(data, &body[1], expect);
    return AS608_OK;
}

/**
 * sensor commands
 */
static as608_status_t cmd_vfy_pwd(uint32_t password)
{
    uint8_t c[5] = {CMD_VFY_PWD};
    put32(&c[1], password);
    return transact(c, sizeof(c), NULL, 0, ACK_TIMEOUT_MS);
}

static as608_status_t cmd_read_sys_para(as608_info_t *out)
{
    const uint8_t c[1] = {CMD_READ_SYS_PARA};
    uint8_t d[16];
    as608_status_t st = transact(c, sizeof(c), d, sizeof(d), ACK_TIMEOUT_MS);
    if (st != AS608_OK)
        return st;

    out->status_reg = get16(&d[0]);
    out->system_id = get16(&d[2]);
    out->capacity = get16(&d[4]);
    out->security_level = get16(&d[6]);
    out->address = get32(&d[8]);
    out->packet_size = (uint16_t)(32u << (get16(&d[12]) & 0x03)); // 32/64/128/256
    out->baud = (uint32_t)get16(&d[14]) * 9600u;
    return AS608_OK;
}

static as608_status_t cmd_gen_img(void)
{
    const uint8_t c[1] = {CMD_GEN_IMG};
    return transact(c, sizeof(c), NULL, 0, ACK_TIMEOUT_MS);
}

static as608_status_t cmd_img2tz(uint8_t buffer_id)
{
    const uint8_t c[2] = {CMD_IMG2TZ, buffer_id};
    return transact(c, sizeof(c), NULL, 0, ACK_TIMEOUT_MS);
}

static as608_status_t cmd_search(uint8_t buffer_id, uint16_t start_page, uint16_t count, uint16_t *id)
{
    uint8_t c[6] = {CMD_SEARCH, buffer_id};
    put16(&c[2], start_page);
    put16(&c[4], count);
    uint8_t d[4]; // page id + score
    as608_status_t st = transact(c, sizeof(c), d, sizeof(d), SEARCH_TIMEOUT_MS);
    if (st == AS608_OK && id)
        *id = get16(&d[0]);
    return st;
}

static as608_status_t cmd_reg_model(void)
{
    const uint8_t c[1] = {CMD_REG_MODEL};
    return transact(c, sizeof(c), NULL, 0, ACK_TIMEOUT_MS);
}

static as608_status_t cmd_store(uint8_t buffer_id, uint16_t id)
{
    uint8_t c[4] = {CMD_STORE, buffer_id};
    put16(&c[2], id);
    return transact(c, sizeof(c), NULL, 0, ACK_TIMEOUT_MS);
}

static as608_status_t cmd_delete_char(uint16_t id, uint16_t count)
{
    uint8_t c[5] = {CMD_DELETE_CHAR};
    put16(&c[1], id);
    put16(&c[3], count);
    return transact(c, sizeof(c), NULL, 0, ACK_TIMEOUT_MS);
}

static as608_status_t cmd_empty(void)
{
    const uint8_t c[1] = {CMD_EMPTY};
    return transact(c, sizeof(c), NULL, 0, ACK_TIMEOUT_MS);
}

static as608_status_t cmd_template_num(uint16_t *count)
{
    const uint8_t c[1] = {CMD_TEMPLATE_NUM};
    uint8_t d[2];
    as608_status_t st = transact(c, sizeof(c), d, sizeof(d), ACK_TIMEOUT_MS);
    if (st == AS608_OK)
        *count = get16(d);
    return st;
}

static as608_status_t cmd_read_index_table(uint8_t page, uint8_t table[32])
{
    const uint8_t c[2] = {CMD_READ_INDEX_TABLE, page};
    return transact(c, sizeof(c), table, 32, ACK_TIMEOUT_MS);
}

/**
 * slot verification
 * order: byte 0 bit 0 = slot 0
 */
static as608_status_t slot_empty_locked(uint16_t finger_id)
{
    if (finger_id >= s.info.capacity)
        return AS608_ERR_BAD_ID;

    uint8_t table[32];
    as608_status_t st = cmd_read_index_table((uint8_t)(finger_id / INDEX_PAGE_BITS), table);
    if (st != AS608_OK)
        return st;

    const uint16_t bit = finger_id % INDEX_PAGE_BITS;
    return (table[bit / 8] >> (bit % 8)) & 1 ? AS608_ERR_SLOT_OCCUPIED : AS608_OK;
}

/**
 * execute while waiting for user
 */
static as608_status_t capture_image(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t total = pdMS_TO_TICKS(timeout_ms);

    for (;;)
    {
        if (s_cancel)
        {
            s_cancel = false;
            return AS608_ERR_CANCELLED;
        }
        const as608_status_t st = cmd_gen_img();
        if (st != AS608_ERR_NO_FINGER)
            return st; // OK, or a real error
        if (elapsed_since(start) >= total)
            return AS608_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/**
 * waiting finger removed from sensor
 */
static as608_status_t wait_finger_removed(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t total = pdMS_TO_TICKS(timeout_ms);
    int clear = 0;

    for (;;)
    {
        if (s_cancel)
        {
            s_cancel = false;
            return AS608_ERR_CANCELLED;
        }
        const as608_status_t st = cmd_gen_img();
        if (st == AS608_ERR_NO_FINGER)
        {
            if (++clear >= REMOVE_CONFIRM_COUNT)
                return AS608_OK;
        }
        else if (st == AS608_OK || st == AS608_ERR_IMAGE_FAIL)
        {
            clear = 0; // finger still there
        }
        else
        {
            return st;
        }
        if (elapsed_since(start) >= total)
            return AS608_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/**
 * enrollment
 */
static as608_status_t step_place_finger(uint32_t timeout_ms, uint16_t *duplicate_id)
{
    s.enroll = ENROLL_IDLE;
    s_cancel = false;

    as608_status_t st = capture_image(timeout_ms);
    if (st != AS608_OK)
        return st;
    st = cmd_img2tz(CHAR_BUFFER_1);
    if (st != AS608_OK)
        return st;

    // is this finger already in the library?
    uint16_t found = 0;
    st = cmd_search(CHAR_BUFFER_1, 0, s.info.capacity, &found);
    if (st == AS608_OK)
    {
        ESP_LOGW(TAG, "Jari sudah terdaftar di slot %u", found);
        if (duplicate_id)
            *duplicate_id = found;
        return AS608_ERR_DUPLICATE;
    }
    if (st != AS608_ERR_NOT_FOUND)
        return st;

    s.enroll = ENROLL_FIRST_CAPTURED;
    return AS608_OK;
}

static as608_status_t step_remove_finger(uint32_t timeout_ms)
{
    // wait for finger remoced from sensor
    const as608_status_t st = wait_finger_removed(timeout_ms);
    if (st == AS608_OK && s.enroll == ENROLL_FIRST_CAPTURED)
        s.enroll = ENROLL_FIRST_REMOVED;
    return st;
}

static as608_status_t step_place_again(uint32_t timeout_ms)
{
    if (s.enroll != ENROLL_FIRST_REMOVED)
        return AS608_ERR_BAD_STATE;

    as608_status_t st = capture_image(timeout_ms);
    if (st != AS608_OK)
        return st; // nothing captured

    // from here a finger is on the sensor, it must be removed before another try
    s.enroll = ENROLL_FIRST_CAPTURED;
    st = cmd_img2tz(CHAR_BUFFER_2);
    if (st != AS608_OK)
        return st;
    st = cmd_reg_model();
    // is missmacth with first scanned finger?
    if (st != AS608_OK)
        return st;

    s.enroll = ENROLL_MODEL_READY;
    return AS608_OK;
}

as608_status_t as608_enroll_step(as608_enroll_step_t step, uint32_t timeout_ms, uint16_t *duplicate_id)
{
    if (!s_ready)
        return AS608_ERR_BAD_STATE;
    if (timeout_ms == 0)
        return AS608_ERR_INVALID_ARG;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    as608_status_t st;
    switch (step)
    {
    case AS608_STEP_PLACE_FINGER:
        st = step_place_finger(timeout_ms, duplicate_id);
        break;
    case AS608_STEP_REMOVE_FINGER:
        st = step_remove_finger(timeout_ms);
        break;
    case AS608_STEP_PLACE_AGAIN:
        st = step_place_again(timeout_ms);
        break;
    default:
        st = AS608_ERR_INVALID_ARG;
        break;
    }
    xSemaphoreGive(s.lock);

    if (st != AS608_OK)
        ESP_LOGW(TAG, "Langkah enroll %d: %s (kode sensor 0x%02x)", (int)step, as608_status_str(st), s.last_code);
    return st;
}

/**
 * template management
 */
as608_status_t as608_save_template(uint16_t finger_id)
{
    if (!s_ready)
        return AS608_ERR_BAD_STATE;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    as608_status_t st;
    if (finger_id >= s.info.capacity)
    {
        st = AS608_ERR_BAD_ID;
    }
    else if (s.enroll != ENROLL_MODEL_READY)
    {
        st = AS608_ERR_BAD_STATE;
    }
    else
    {
        // lock empty slot
        st = slot_empty_locked(finger_id);
        if (st == AS608_OK)
            st = cmd_store(CHAR_BUFFER_1, finger_id);
        if (st == AS608_OK)
            s.enroll = ENROLL_IDLE;
    }
    xSemaphoreGive(s.lock);

    if (st != AS608_OK)
        ESP_LOGW(TAG, "Simpan template slot %u: %s (kode sensor 0x%02x)", finger_id, as608_status_str(st), s.last_code);
    else
        ESP_LOGI(TAG, "Template tersimpan di slot %u", finger_id);
    return st;
}

as608_status_t as608_delete_template(uint16_t finger_id)
{
    if (!s_ready)
        return AS608_ERR_BAD_STATE;
    if (finger_id >= s.info.capacity)
        return AS608_ERR_BAD_ID;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    const as608_status_t st = cmd_delete_char(finger_id, 1);
    xSemaphoreGive(s.lock);

    if (st != AS608_OK)
        ESP_LOGW(TAG, "Hapus slot %u: %s (kode sensor 0x%02x)", finger_id, as608_status_str(st), s.last_code);
    else
        ESP_LOGI(TAG, "Slot %u dihapus", finger_id);
    return st;
}

as608_status_t as608_empty_database(void)
{
    if (!s_ready)
        return AS608_ERR_BAD_STATE;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    const as608_status_t st = cmd_empty();
    xSemaphoreGive(s.lock);

    if (st != AS608_OK)
        ESP_LOGW(TAG, "Kosongkan library: %s (kode sensor 0x%02x)", as608_status_str(st), s.last_code);
    else
        ESP_LOGI(TAG, "Library sidik jari dikosongkan");
    return st;
}

as608_status_t as608_check_slot_empty(uint16_t finger_id)
{
    if (!s_ready)
        return AS608_ERR_BAD_STATE;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    const as608_status_t st = slot_empty_locked(finger_id);
    xSemaphoreGive(s.lock);
    return st;
}

as608_status_t as608_template_count(uint16_t *count)
{
    if (count == NULL)
        return AS608_ERR_INVALID_ARG;
    if (!s_ready)
        return AS608_ERR_BAD_STATE;

    xSemaphoreTake(s.lock, portMAX_DELAY);
    const as608_status_t st = cmd_template_num(count);
    xSemaphoreGive(s.lock);
    return st;
}

as608_status_t as608_get_info(as608_info_t *out)
{
    if (out == NULL)
        return AS608_ERR_INVALID_ARG;
    if (!s_ready)
        return AS608_ERR_BAD_STATE;
    *out = s.info; // read only after init
    return AS608_OK;
}

/**
 * init
 */
static as608_status_t setup_uart(const as608_config_t *cfg)
{
    const uart_config_t uc = {
        .baud_rate = (int)cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (!uart_is_driver_installed(cfg->uart_num))
    {
        esp_err_t err = uart_driver_install(cfg->uart_num, RX_BUF_SIZE, 0, 0, NULL, 0);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Gagal install driver UART: %s", esp_err_to_name(err));
            return AS608_ERR_INTERNAL;
        }
    }

    esp_err_t err = uart_param_config(cfg->uart_num, &uc);
    if (err == ESP_OK)
        err = uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    // is uart port got problems?
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Gagal konfigurasi UART: %s", esp_err_to_name(err));
        return AS608_ERR_INTERNAL;
    }
    return AS608_OK;
}

as608_status_t as608_init(const as608_config_t *cfg)
{
    if (s_ready)
        return AS608_OK;
    if (cfg == NULL || cfg->baud == 0)
        return AS608_ERR_INVALID_ARG;

    if (s.lock == NULL)
    {
        s.lock = xSemaphoreCreateMutex();
        if (s.lock == NULL)
            return AS608_ERR_INTERNAL;
    }
    s.cfg = *cfg;
    s.enroll = ENROLL_IDLE;
    s_cancel = false;

    as608_status_t st = setup_uart(cfg);
    if (st != AS608_OK)
        return st;

    // WAK implement ready, tapi nanti dulu
    if (cfg->wake_pin != GPIO_NUM_NC)
    {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << cfg->wake_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&io) == ESP_OK)
            ESP_LOGI(TAG, "Pin WAK (GPIO %d) level awal: %d", (int)cfg->wake_pin, gpio_get_level(cfg->wake_pin));
    }

    // retry till sensor ready
    for (int i = 0; i < VFY_RETRIES; i++)
    {
        st = cmd_vfy_pwd(cfg->password);
        if (st == AS608_OK || st == AS608_ERR_WRONG_PASSWORD)
            break;
        vTaskDelay(pdMS_TO_TICKS(VFY_RETRY_DELAY_MS));
    }
    if (st != AS608_OK)
    {
        ESP_LOGE(TAG, "Sensor tidak terverifikasi: %s. Periksa kabel TX/RX, level shifter, daya 5V, dan baud %lu",
                 as608_status_str(st), (unsigned long)cfg->baud);
        return st;
    }

    st = cmd_read_sys_para(&s.info);
    if (st != AS608_OK)
    {
        ESP_LOGE(TAG, "Gagal membaca parameter sistem: %s", as608_status_str(st));
        return st;
    }
    // is enough capacity?
    if (s.info.capacity == 0)
    {
        ESP_LOGE(TAG, "Sensor melaporkan kapasitas 0");
        return AS608_ERR_SENSOR;
    }

    s_ready = true;
    ESP_LOGI(TAG, "AS608 siap: kapasitas=%u slot, security=%u, paket=%u byte, baud=%lu",
             s.info.capacity, s.info.security_level, s.info.packet_size, (unsigned long)s.info.baud);
    return AS608_OK;
}