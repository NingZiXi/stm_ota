/**
 * @file    stm_ota.c
 * @brief   STM32 OTA：通过 ESP32 HTTP GET 拉固件 chunk 写本地 flash
 */

#include "stm_ota.h"
#include "stm_log.h"

/* A/B 槽边界由应用与 Bootloader 共用，避免 OTA 库重复维护地址。 */
#include "../../../common/boot_state_protocol.h"

#include "esp_at_client.h"
#include "esp_at_tcp.h"

#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"             // 内部 HAL：stm_ota 限定 STM32F4

// 兼容旧配置的默认暂存地址；F407 片内 Flash 范围校验会拒绝该地址，
// 调用方必须显式传入 BOOT_SLOT_A_BASE 或 BOOT_SLOT_B_BASE。
#define STM_OTA_DEFAULT_DOWNLOAD_ADDR  0x08080000U
#define STM_OTA_DEFAULT_CHUNK          1024U
#define STM_OTA_MAX_CHUNK              1400U
#define STM_OTA_HTTP_RESP_BUF          4096U
#define STM_OTA_LOG_EVERY_CHUNKS       16U
#define STM_OTA_PREFLIGHT_BYTES        8U
#define STM_OTA_HTTP_TIMEOUT_MS        8000U
#define STM_OTA_HTTP_RETRY_COUNT       3U
#define STM_OTA_HTTP_RETRY_DELAY_MS    100U
#define STM_OTA_FLAG_ADDR              0x40024000U   // 备份寄存器：实际放 (BKP_BASE + 0x04)
#define STM_OTA_FLAG_MAGIC             0x0FADE001U
#define STM_OTA_RESUME_MAGIC           0x3152544FU   // "OTR1"

/*
 * 测试故障注入：仅测试构建可将其设为 1-based 的 flash_write() 调用序号，
 * 该次写入返回失败而不触碰 Flash。正式构建保持为 0，完全关闭注入。
 */
#ifndef CONFIG_OTA_TEST_FLASH_FAIL_CHUNK
#define CONFIG_OTA_TEST_FLASH_FAIL_CHUNK 0U
#endif

/* BKP3R..BKP10R 保存 OTA 断点；BKP1R/BKP2R 继续由 Bootloader 状态协议使用。 */
typedef struct {
    uint32_t download_addr;
    uint32_t total_size;
    uint32_t package_crc32;
    uint32_t chunk_size;
    uint32_t next_offset;
    uint32_t prefix_crc32;
} stm_ota_resume_state_t;

// Flash 工具（直接调用 HAL，仅支持 STM32F4）。
static int flash_unlock(void)
{
    return HAL_FLASH_Unlock() == HAL_OK ? 0 : -1;
}

static int flash_lock(void)
{
    HAL_FLASH_Lock();
    return 0;
}

// 根据绝对地址计算 STM32F407 Flash sector。
static uint32_t addr_to_sector(uint32_t addr)
{
    if (addr < 0x08004000U) return FLASH_SECTOR_0;
    if (addr < 0x08008000U) return FLASH_SECTOR_1;
    if (addr < 0x0800C000U) return FLASH_SECTOR_2;
    if (addr < 0x08010000U) return FLASH_SECTOR_3;
    if (addr < 0x08020000U) return FLASH_SECTOR_4;
    if (addr < 0x08040000U) return FLASH_SECTOR_5;
    if (addr < 0x08060000U) return FLASH_SECTOR_6;
    if (addr < 0x08080000U) return FLASH_SECTOR_7;
    if (addr < 0x080A0000U) return FLASH_SECTOR_8;
    if (addr < 0x080C0000U) return FLASH_SECTOR_9;
    if (addr < 0x080E0000U) return FLASH_SECTOR_10;
    return FLASH_SECTOR_11;
}

static int flash_erase(uint32_t addr, uint32_t size)
{
    /* 防御性检查：调用方应已验证，但这里也不能接受空范围或整数回绕。 */
    if (size == 0U || addr > (UINT32_MAX - (size - 1U))) {
        return -1;
    }
    uint32_t start_sector = addr_to_sector(addr);
    uint32_t end_sector = addr_to_sector(addr + size - 1);
    FLASH_EraseInitTypeDef erase = {0};
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = start_sector;
    erase.NbSectors = end_sector - start_sector + 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    uint32_t err = 0;
    return (HAL_FLASHEx_Erase(&erase, &err) == HAL_OK) ? 0 : -1;
}

static int flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    static uint32_t s_flash_write_calls;

    if (data == NULL && len != 0U) {
        return -1;
    }

    s_flash_write_calls++;
#if CONFIG_OTA_TEST_FLASH_FAIL_CHUNK > 0
    if (s_flash_write_calls == CONFIG_OTA_TEST_FLASH_FAIL_CHUNK) {
        LOGW("ota", "test fault injection: Flash write call %lu failed",
             (unsigned long)s_flash_write_calls);
        return -1;
    }
#endif

    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t remaining = len - i;
        uint32_t word = 0xFFFFFFFFU;
        uint32_t copy = remaining < 4U ? remaining : 4U;
        memcpy(&word, data + i, copy);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word) != HAL_OK) {
            return -1;
        }
    }
    return 0;
}

/*
 * 只接受从槽位基址开始的连续写入。这样既能保护 Bootloader 区域，也能
 * 防止一次 OTA 跨过 A/B 边界或写入 B 槽末尾保留的 32 KiB。
 *
 * 返回值为 0 表示合法，并通过 aligned_size 返回按 Flash 字写入所需的
 * 补齐长度。Flash 最后一个 word 可能包含 0xFF 填充，因此补齐后的范围也
 * 必须仍在槽位包上限内。
 */
static int validate_download_range(uint32_t addr, uint32_t total_size,
                                   uint32_t *aligned_size)
{
    uint32_t slot_limit;

    if (aligned_size == NULL || total_size == 0U) {
        return -1;
    }

    if (addr == BOOT_SLOT_A_BASE) {
        slot_limit = BOOT_SLOT_A_BASE + BOOT_SLOT_PACKAGE_SIZE;
    } else if (addr == BOOT_SLOT_B_BASE) {
        slot_limit = BOOT_SLOT_B_BASE + BOOT_SLOT_PACKAGE_SIZE;
    } else {
        return -1;
    }

    /* 槽位基址天然 4 字节对齐；保留检查以防协议地址以后被修改。 */
    if ((addr & 0x3U) != 0U || addr >= slot_limit
        || total_size > (UINT32_MAX - addr)) {
        return -1;
    }

    const uint32_t end = addr + total_size;
    if (end > slot_limit) {
        return -1;
    }

    /* flash_write() 按 32 位字编程，检查补齐后的最后一个字。 */
    if (total_size > (UINT32_MAX - 3U)) {
        return -1;
    }
    const uint32_t padded = (total_size + 3U) & ~3U;
    if (padded > (slot_limit - addr)) {
        return -1;
    }

    *aligned_size = padded;
    return 0;
}

// 更新 CRC32 状态。
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint32_t k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320U & -(crc & 1));
        }
    }
    return crc;
}

// 计算 CRC32。
uint32_t stm_ota_crc32(const uint8_t *data, uint32_t len)
{
    return crc32_update(0xFFFFFFFFU, data, len) ^ 0xFFFFFFFFU;
}

static uint32_t resume_state_crc32(const stm_ota_resume_state_t *state)
{
    return stm_ota_crc32((const uint8_t *)state, sizeof *state);
}

static void resume_backup_write_begin(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
}

static void resume_backup_write_end(void)
{
    HAL_PWR_DisableBkUpAccess();
}

static void resume_state_clear(void)
{
    resume_backup_write_begin();
    WRITE_REG(RTC->BKP3R, 0U);
    __DSB();
    WRITE_REG(RTC->BKP4R, 0U);
    WRITE_REG(RTC->BKP5R, 0U);
    WRITE_REG(RTC->BKP6R, 0U);
    WRITE_REG(RTC->BKP7R, 0U);
    WRITE_REG(RTC->BKP8R, 0U);
    WRITE_REG(RTC->BKP9R, 0U);
    WRITE_REG(RTC->BKP10R, 0U);
    resume_backup_write_end();
}

/* 魔数最后写入；若复位发生在更新中间，下一次会把该断点视为无效。 */
static void resume_state_save(const stm_ota_resume_state_t *state)
{
    const uint32_t state_crc = resume_state_crc32(state);

    resume_backup_write_begin();
    WRITE_REG(RTC->BKP3R, 0U);
    __DSB();
    WRITE_REG(RTC->BKP4R, state->download_addr);
    WRITE_REG(RTC->BKP5R, state->total_size);
    WRITE_REG(RTC->BKP6R, state->package_crc32);
    WRITE_REG(RTC->BKP7R, state->chunk_size);
    WRITE_REG(RTC->BKP8R, state->next_offset);
    WRITE_REG(RTC->BKP9R, state->prefix_crc32);
    WRITE_REG(RTC->BKP10R, state_crc);
    __DSB();
    WRITE_REG(RTC->BKP3R, STM_OTA_RESUME_MAGIC);
    __DSB();
    resume_backup_write_end();
}

static bool resume_state_load(uint32_t addr, uint32_t total,
                              uint32_t package_crc, uint32_t chunk,
                              stm_ota_resume_state_t *state,
                              bool *state_present)
{
    if (state == NULL || state_present == NULL) return false;

    resume_backup_write_begin();
    *state_present = READ_REG(RTC->BKP3R) == STM_OTA_RESUME_MAGIC;
    if (!*state_present) {
        resume_backup_write_end();
        return false;
    }

    state->download_addr = READ_REG(RTC->BKP4R);
    state->total_size = READ_REG(RTC->BKP5R);
    state->package_crc32 = READ_REG(RTC->BKP6R);
    state->chunk_size = READ_REG(RTC->BKP7R);
    state->next_offset = READ_REG(RTC->BKP8R);
    state->prefix_crc32 = READ_REG(RTC->BKP9R);
    const uint32_t saved_crc = READ_REG(RTC->BKP10R);
    resume_backup_write_end();

    if (saved_crc != resume_state_crc32(state)
        || state->download_addr != addr
        || state->total_size != total
        || state->package_crc32 != package_crc
        || state->chunk_size != chunk
        || state->next_offset > total
        || (state->next_offset != total
            && (state->next_offset % chunk) != 0U)) {
        return false;
    }

    const uint32_t flash_crc = stm_ota_crc32((const uint8_t *)addr,
                                             state->next_offset);
    return flash_crc == state->prefix_crc32;
}

// 解析 URL：http://主机[:端口]/路径。
static int parse_url(const char *url, char *host, uint16_t host_sz,
                     uint16_t *port, char *path, uint16_t path_sz)
{
    static const char scheme[] = "http://";
    if (url == NULL || host == NULL || host_sz < 2U || port == NULL
        || path == NULL || path_sz < 2U
        || strncmp(url, scheme, sizeof scheme - 1U) != 0) {
        return -1;
    }
    const char *p = url + sizeof scheme - 1U;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t h_len = (size_t)(colon - p);
        if (h_len >= host_sz) return -1;
        memcpy(host, p, h_len);
        host[h_len] = '\0';
        const char *port_text = colon + 1;
        const char *port_end = slash ? slash : p + strlen(p);
        uint32_t parsed_port = 0U;
        if (port_text == port_end) return -1;
        while (port_text < port_end) {
            if (*port_text < '0' || *port_text > '9') return -1;
            parsed_port = parsed_port * 10U + (uint32_t)(*port_text++ - '0');
            if (parsed_port > UINT16_MAX) return -1;
        }
        if (parsed_port == 0U) return -1;
        *port = (uint16_t)parsed_port;
        p = slash ? slash : p + strlen(p);
    } else {
        const char *end = slash ? slash : (p + strlen(p));
        size_t h_len = (size_t)(end - p);
        if (h_len >= host_sz) return -1;
        memcpy(host, p, h_len);
        host[h_len] = '\0';
        *port = 80;
        p = end;
    }
    if (!p || !*p) {
        p = "/";
    }
    strncpy(path, p, path_sz - 1);
    path[path_sz - 1] = '\0';
    return 0;
}

// 文件级 static：供 stm_ota_download 结束时关闭连接。
static esp_at_tcp_t s_tcp;
static bool s_connected = false;
static char s_host[64];
static char s_path[64];
static uint16_t s_port = 0;

// 查找 HTTP 头结束标记 "\r\n\r\n"，返回包含标记的结束偏移。
static int find_header_end(const uint8_t *resp, uint32_t len)
{
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' &&
            resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            return (int)(i + 4);
        }
    }
    return -1;
}

static int hex_digit_value(uint8_t ch)
{
    if (ch >= '0' && ch <= '9') return (int)(ch - '0');
    if (ch >= 'A' && ch <= 'F') return (int)(ch - 'A') + 10;
    if (ch >= 'a' && ch <= 'f') return (int)(ch - 'a') + 10;
    return -1;
}

static uint8_t ascii_lower(uint8_t ch)
{
    return (ch >= 'A' && ch <= 'Z') ? (uint8_t)(ch + ('a' - 'A')) : ch;
}

static bool header_name_equal(const uint8_t *text, const char *name,
                              uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        if (ascii_lower(text[i]) != ascii_lower((uint8_t)name[i])) return false;
    }
    return true;
}

/* 从完整 HTTP 头中读取一个最多 8 位的十六进制 uint32。 */
static int parse_hex_header(const uint8_t *resp, uint32_t header_len,
                            const char *name, uint32_t *value)
{
    if (resp == NULL || name == NULL || value == NULL) return -1;

    const uint32_t name_len = (uint32_t)strlen(name);
    uint32_t line_start = 0U;
    while (line_start + 1U < header_len) {
        uint32_t line_end = line_start;
        while (line_end + 1U < header_len
               && !(resp[line_end] == '\r' && resp[line_end + 1U] == '\n')) {
            line_end++;
        }
        if (line_end + 1U >= header_len) break;

        if (line_end > line_start + name_len
            && header_name_equal(resp + line_start, name, name_len)
            && resp[line_start + name_len] == ':') {
            uint32_t pos = line_start + name_len + 1U;
            while (pos < line_end && (resp[pos] == ' ' || resp[pos] == '\t')) pos++;

            /* 兼容 X-CRC32 的纯十六进制和 Version-Code 的 0x 前缀。 */
            if (pos + 2U <= line_end && resp[pos] == '0'
                && (resp[pos + 1U] == 'x' || resp[pos + 1U] == 'X')) {
                pos += 2U;
            }

            uint32_t parsed = 0U;
            uint32_t digits = 0U;
            while (pos < line_end) {
                const int digit = hex_digit_value(resp[pos++]);
                if (digit < 0 || digits >= 8U) return -1;
                parsed = (parsed << 4) | (uint32_t)digit;
                digits++;
            }
            if (digits == 0U) return -1;
            *value = parsed;
            return 0;
        }
        line_start = line_end + 2U;
    }
    return -1;
}

/* 从完整 HTTP 头中读取一个十进制 uint32。 */
static int parse_decimal_header(const uint8_t *resp, uint32_t header_len,
                                const char *name, uint32_t *value)
{
    if (resp == NULL || name == NULL || value == NULL) return -1;

    const uint32_t name_len = (uint32_t)strlen(name);
    uint32_t line_start = 0U;
    while (line_start + 1U < header_len) {
        uint32_t line_end = line_start;
        while (line_end + 1U < header_len
               && !(resp[line_end] == '\r' && resp[line_end + 1U] == '\n')) {
            line_end++;
        }
        if (line_end + 1U >= header_len) break;

        if (line_end > line_start + name_len
            && header_name_equal(resp + line_start, name, name_len)
            && resp[line_start + name_len] == ':') {
            uint32_t pos = line_start + name_len + 1U;
            while (pos < line_end && (resp[pos] == ' ' || resp[pos] == '\t')) pos++;

            uint32_t parsed = 0U;
            uint32_t digits = 0U;
            while (pos < line_end && resp[pos] >= '0' && resp[pos] <= '9') {
                const uint32_t digit = (uint32_t)(resp[pos++] - '0');
                if (parsed > (UINT32_MAX - digit) / 10U) return -1;
                parsed = parsed * 10U + digit;
                digits++;
            }
            if (digits == 0U || pos != line_end) return -1;
            *value = parsed;
            return 0;
        }
        line_start = line_end + 2U;
    }
    return -1;
}

/* 读取 X-Firmware-Slot，只接受单字符 A/B。 */
static int parse_slot_header(const uint8_t *resp, uint32_t header_len,
                             uint8_t *slot)
{
    static const char name[] = "X-Firmware-Slot";
    const uint32_t name_len = (uint32_t)strlen(name);
    uint32_t line_start = 0U;

    if (resp == NULL || slot == NULL) return -1;

    while (line_start + 1U < header_len) {
        uint32_t line_end = line_start;
        while (line_end + 1U < header_len
               && !(resp[line_end] == '\r' && resp[line_end + 1U] == '\n')) {
            line_end++;
        }
        if (line_end + 1U >= header_len) break;

        if (line_end > line_start + name_len
            && header_name_equal(resp + line_start, name, name_len)
            && resp[line_start + name_len] == ':') {
            uint32_t pos = line_start + name_len + 1U;
            while (pos < line_end && (resp[pos] == ' ' || resp[pos] == '\t')) pos++;
            if (pos + 1U != line_end) return -1;
            if (resp[pos] == 'A' || resp[pos] == 'a') {
                *slot = 0U;
                return 0;
            }
            if (resp[pos] == 'B' || resp[pos] == 'b') {
                *slot = 1U;
                return 0;
            }
            return -1;
        }
        line_start = line_end + 2U;
    }
    return -1;
}

/* 解析 Content-Range: bytes <start>-<end>/<total>。 */
static int parse_content_range(const uint8_t *resp, uint32_t header_len,
                               uint32_t *start, uint32_t *end,
                               uint32_t *total)
{
    static const char name[] = "Content-Range";
    const uint32_t name_len = (uint32_t)strlen(name);
    uint32_t line_start = 0U;

    if (resp == NULL || start == NULL || end == NULL || total == NULL) return -1;

    while (line_start + 1U < header_len) {
        uint32_t line_end = line_start;
        while (line_end + 1U < header_len
               && !(resp[line_end] == '\r' && resp[line_end + 1U] == '\n')) {
            line_end++;
        }
        if (line_end + 1U >= header_len) break;

        if (line_end > line_start + name_len
            && header_name_equal(resp + line_start, name, name_len)
            && resp[line_start + name_len] == ':') {
            uint32_t pos = line_start + name_len + 1U;
            while (pos < line_end && (resp[pos] == ' ' || resp[pos] == '\t')) pos++;
            if (pos + 6U > line_end || memcmp(resp + pos, "bytes ", 6U) != 0) return -1;
            pos += 6U;

            uint32_t values[3] = {0U, 0U, 0U};
            for (uint32_t i = 0U; i < 3U; i++) {
                uint32_t digits = 0U;
                while (pos < line_end && resp[pos] >= '0' && resp[pos] <= '9') {
                    const uint32_t digit = (uint32_t)(resp[pos++] - '0');
                    if (values[i] > (0xFFFFFFFFU - digit) / 10U) return -1;
                    values[i] = values[i] * 10U + digit;
                    digits++;
                }
                if (digits == 0U) return -1;
                if (i == 0U && pos < line_end && resp[pos] == '-') pos++;
                else if (i == 1U && pos < line_end && resp[pos] == '/') pos++;
                else if (i < 2U) return -1;
            }
            if (pos != line_end) return -1;
            *start = values[0];
            *end = values[1];
            *total = values[2];
            return 0;
        }
        line_start = line_end + 2U;
    }
    return -1;
}

static void http_close(void)
{
    if (s_connected) {
        esp_at_tcp_close(&s_tcp);
        s_connected = false;
    }
}

// 通过原始 TCP 长连接和 Range 头拉取一个 HTTP 分片。
// 首次调用建立 host/port/path/tcp 连接，后续调用复用连接。
static int http_pull_chunk(const char *base_url, uint32_t offset, uint32_t want,
                           uint8_t *out, uint32_t *got,
                           uint32_t *package_crc32,
                           stm_ota_image_info_t *image_info)
{
    if (base_url == NULL || out == NULL || got == NULL
        || package_crc32 == NULL || want == 0U) {
        return -1;
    }

    if (!s_connected) {
        if (parse_url(base_url, s_host, sizeof s_host, &s_port, s_path, sizeof s_path) != 0) {
            LOGE("ota", "bad url: %s", base_url);
            return -1;
        }
        if (esp_at_tcp_connect(&s_tcp, s_host, s_port, 8000) != STM_OK) {
            LOGE("ota", "TCP connect %s:%u failed", s_host, s_port);
            return -1;
        }
        s_connected = true;
    }

    if (esp_at_tcp_http_get_range(&s_tcp, s_host, s_path,
                                    offset, want, STM_OTA_HTTP_TIMEOUT_MS) != STM_OK) {
        LOGE("ota", "HTTP GET range %lu-%lu failed", offset, offset + want - 1);
        esp_at_tcp_close(&s_tcp);
        s_connected = false;
        return -1;
    }

    // +IPD 帧就是 HTTP 响应：状态行 + 头 + \r\n\r\n + body。
    static uint8_t s_resp[STM_OTA_HTTP_RESP_BUF];       // HTTP 头 + 最大 OTA 分片
    uint32_t got_bytes = 0;
    if (esp_at_tcp_recv_body(&s_tcp, s_resp, sizeof s_resp, &got_bytes, 12000) != STM_OK) {
        LOGE("ota", "recv body failed");
        esp_at_tcp_close(&s_tcp);
        s_connected = false;
        return -1;
    }

    int hdr_end = find_header_end(s_resp, got_bytes);
    if (hdr_end < 0) {
        LOGE("ota", "HTTP response header not complete (%u bytes)", got_bytes);
        esp_at_tcp_close(&s_tcp);
        s_connected = false;
        return -1;
    }

    if (got_bytes < 12U
        || (memcmp(s_resp, "HTTP/1.1 206", 12U) != 0
            && memcmp(s_resp, "HTTP/1.0 206", 12U) != 0)) {
        LOGE("ota", "HTTP Range response is not 206");
        esp_at_tcp_close(&s_tcp);
        s_connected = false;
        return -1;
    }
    if (parse_hex_header(s_resp, (uint32_t)hdr_end,
                         "X-CRC32", package_crc32) != 0) {
        LOGE("ota", "HTTP response missing valid X-CRC32");
        esp_at_tcp_close(&s_tcp);
        s_connected = false;
        return -1;
    }

    uint32_t body_off = (uint32_t)hdr_end;
    uint32_t body_len = got_bytes - body_off;
    uint32_t range_start = 0U;
    uint32_t range_end = 0U;
    uint32_t range_total = 0U;
    if (parse_content_range(s_resp, (uint32_t)hdr_end,
                            &range_start, &range_end, &range_total) != 0
        || range_start != offset
        || range_end < range_start
        || (range_end - range_start + 1U) != want
        || body_len != want) {
        LOGE("ota", "HTTP range mismatch: req=%lu-%lu body=%lu "
             "content-range=%lu-%lu/%lu",
             (unsigned long)offset,
             (unsigned long)(offset + want - 1U),
             (unsigned long)body_len,
             (unsigned long)range_start,
             (unsigned long)range_end,
             (unsigned long)range_total);
        esp_at_tcp_close(&s_tcp);
        s_connected = false;
        return -1;
    }

    if (image_info != NULL) {
        uint32_t image_version = 0U;
        uint32_t package_size = 0U;
        uint8_t target_slot = 0xFFU;
        if (parse_hex_header(s_resp, (uint32_t)hdr_end,
                             "X-Firmware-Version-Code", &image_version) != 0
            || parse_decimal_header(s_resp, (uint32_t)hdr_end,
                                    "X-Firmware-Size", &package_size) != 0
            || parse_slot_header(s_resp, (uint32_t)hdr_end, &target_slot) != 0
            || package_size != range_total) {
            LOGE("ota", "HTTP response missing or inconsistent image metadata");
            esp_at_tcp_close(&s_tcp);
            s_connected = false;
            return -1;
        }
        image_info->image_version = image_version;
        image_info->package_size = package_size;
        image_info->package_crc32 = *package_crc32;
        image_info->target_slot = target_slot;
    }

    memcpy(out, s_resp + body_off, body_len);
    *got = body_len;
    LOGI("ota", "range %lu-%lu body=%lu first=%02X%02X%02X%02X "
         "last=%02X%02X%02X%02X",
         (unsigned long)range_start,
         (unsigned long)range_end,
         (unsigned long)body_len,
         (unsigned)s_resp[body_off + 0U],
         (unsigned)s_resp[body_off + 1U],
         (unsigned)s_resp[body_off + 2U],
         (unsigned)s_resp[body_off + 3U],
         (unsigned)s_resp[body_off + body_len - 4U],
         (unsigned)s_resp[body_off + body_len - 3U],
         (unsigned)s_resp[body_off + body_len - 2U],
         (unsigned)s_resp[body_off + body_len - 1U]);
    return 0;
}

/*
 * ESP-AT 的 TCP 连接可能在长时间 Range 下载中瞬时关闭，或短暂拒绝
 * CIPSEND。只重试当前分片，每次失败先关闭旧连接再重新建立；上层仍然
 * 负责 CRC 校验和最终失败时的断点状态清理。
 */
static int http_pull_chunk_with_retry(const char *base_url,
                                      uint32_t offset,
                                      uint32_t want,
                                      uint8_t *out,
                                      uint32_t *got,
                                      uint32_t *package_crc32,
                                      stm_ota_image_info_t *image_info)
{
    for (uint32_t attempt = 1U; attempt <= STM_OTA_HTTP_RETRY_COUNT; attempt++) {
        if (http_pull_chunk(base_url, offset, want, out, got,
                            package_crc32, image_info) == 0) {
            return 0;
        }

        http_close();
        if (attempt < STM_OTA_HTTP_RETRY_COUNT) {
            LOGW("ota", "range %lu-%lu retry %lu/%lu",
                 (unsigned long)offset,
                 (unsigned long)(offset + want - 1U),
                 (unsigned long)(attempt + 1U),
                 (unsigned long)STM_OTA_HTTP_RETRY_COUNT);
            HAL_Delay(STM_OTA_HTTP_RETRY_DELAY_MS);
        }
    }
    return -1;
}

stm_err_t stm_ota_probe(const char *url, stm_ota_image_info_t *info)
{
    if (url == NULL || url[0] == '\0' || info == NULL) {
        return STM_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof *info);
    info->target_slot = 0xFFU;

    uint8_t probe[STM_OTA_PREFLIGHT_BYTES];
    uint32_t got = 0U;
    uint32_t package_crc = 0U;
    const int rc = http_pull_chunk_with_retry(url, 0U, sizeof probe, probe, &got,
                                              &package_crc, info);
    http_close();
    if (rc != 0 || got != sizeof probe) {
        return STM_ERR_IO;
    }

    LOGI("ota", "preflight slot=%c version=0x%08lX size=%lu crc=0x%08lX",
         info->target_slot == 0U ? 'A' : info->target_slot == 1U ? 'B' : '?',
         (unsigned long)info->image_version,
         (unsigned long)info->package_size,
         (unsigned long)info->package_crc32);
    return STM_OK;
}

stm_err_t stm_ota_download(const stm_ota_config_t *cfg)
{
    if (!cfg || !cfg->url || cfg->url[0] == '\0') {
        return STM_ERR_INVALID_ARG;
    }

    uint32_t addr = cfg->download_addr ? cfg->download_addr : STM_OTA_DEFAULT_DOWNLOAD_ADDR;
    uint32_t chunk = cfg->chunk_size ? cfg->chunk_size : STM_OTA_DEFAULT_CHUNK;
    uint32_t total = cfg->total_size;
    static uint8_t s_buf[STM_OTA_MAX_CHUNK];           // 固定静态下载缓冲，不依赖 heap。
    uint32_t programmed_size = 0U;

    /* 所有参数先校验，再解锁/擦除 Flash，避免错误配置造成破坏性副作用。 */
    if (validate_download_range(addr, total, &programmed_size) != 0
        || chunk < 4U || (chunk & 0x3U) != 0U || chunk > sizeof s_buf) {
        return STM_ERR_INVALID_ARG;
    }

    uint32_t done = 0;
    uint32_t download_crc = 0xFFFFFFFFU;
    uint32_t package_crc = cfg->crc32_expected;
    bool package_crc_known = cfg->crc32_expected != 0U;
    bool resume_present = false;
    bool resume_accepted = false;
    stm_ota_resume_state_t resume = {0};

    if (package_crc_known
        && resume_state_load(addr, total, package_crc, chunk,
                             &resume, &resume_present)) {
        done = resume.next_offset;
        download_crc = resume.prefix_crc32 ^ 0xFFFFFFFFU;
        resume_accepted = true;
        LOGI("ota", "resume accepted offset=%lu/%lu prefix_crc=0x%08lX",
             (unsigned long)done, (unsigned long)total,
             (unsigned long)resume.prefix_crc32);
    } else if (resume_present) {
        LOGW("ota", "resume state rejected, restart from offset 0");
        resume_state_clear();
    }

    if (flash_unlock() != 0) {
        resume_state_clear();
        return STM_ERR_IO;
    }
    if (!resume_accepted) {
        if (flash_erase(addr, programmed_size) != 0) {
            flash_lock();
            resume_state_clear();
            return STM_ERR_IO;
        }
        if (package_crc_known) {
            resume = (stm_ota_resume_state_t) {
                .download_addr = addr,
                .total_size = total,
                .package_crc32 = package_crc,
                .chunk_size = chunk,
                .next_offset = 0U,
                .prefix_crc32 = 0U,
            };
            resume_state_save(&resume);
        }
    }

    while (done < total) {
        uint32_t want = total - done;
        if (want > chunk) want = chunk;

        uint32_t got = 0;
        uint32_t response_crc = 0U;
        if (http_pull_chunk_with_retry(cfg->url, done, want, s_buf, &got,
                                       &response_crc, NULL) != 0) {
            flash_lock();
            http_close();
            resume_state_clear();
            return STM_ERR_IO;
        }
        if (!package_crc_known) {
            package_crc = response_crc;
            package_crc_known = true;
            LOGI("ota", "server package crc=0x%08lX", (unsigned long)package_crc);
        } else if (response_crc != package_crc) {
            LOGE("ota", "package CRC header changed: 0x%08lX -> 0x%08lX",
                 (unsigned long)package_crc, (unsigned long)response_crc);
            flash_lock();
            http_close();
            resume_state_clear();
            return STM_ERR_VERIFY;
        }
        if (got == 0) break;

        uint32_t chunk_crc = stm_ota_crc32(s_buf, got);
        download_crc = crc32_update(download_crc, s_buf, got);
        if (flash_write(addr + done, s_buf, got) != 0) {
            flash_lock();
            http_close();
            resume_state_clear();
            return STM_ERR_IO;
        }
        uint32_t flash_crc = stm_ota_crc32((const uint8_t *)(addr + done), got);
        if (flash_crc != chunk_crc) {
            flash_lock();
            http_close();
            resume_state_clear();
            return STM_ERR_IO;
        }
        const uint32_t chunk_start = done;
        done += got;
        if (chunk_start == 0U || done >= total
            || ((done / chunk) % STM_OTA_LOG_EVERY_CHUNKS) == 0U) {
            LOGI("ota", "chunk %lu len=%lu rx_crc=0x%08lX flash_crc=0x%08lX "
                 "progress=%lu/%lu",
                 (unsigned long)chunk_start, (unsigned long)got,
                 (unsigned long)chunk_crc, (unsigned long)flash_crc,
                 (unsigned long)done, (unsigned long)total);
        }

        if (package_crc_known) {
            resume.download_addr = addr;
            resume.total_size = total;
            resume.package_crc32 = package_crc;
            resume.chunk_size = chunk;
            resume.next_offset = done;
            resume.prefix_crc32 = download_crc ^ 0xFFFFFFFFU;
            resume_state_save(&resume);
        }

        if (cfg->progress_cb) {
            cfg->progress_cb(done, total, cfg->progress_user);
        }
    }
    flash_lock();

    http_close();

    if (done != total) {
        resume_state_clear();
        return STM_ERR_IO;
    }

    if (package_crc_known) {
        uint32_t rx_crc = download_crc ^ 0xFFFFFFFFU;
        uint32_t flash_crc = stm_ota_crc32((const uint8_t *)addr, total);
        LOGI("ota", "final rx_crc=0x%08lX flash_crc=0x%08lX expected=0x%08lX",
             (unsigned long)rx_crc, (unsigned long)flash_crc,
             (unsigned long)package_crc);
        if (rx_crc != package_crc || flash_crc != package_crc) {
            resume_state_clear();
            return STM_ERR_VERIFY;
        }
    } else {
        resume_state_clear();
        return STM_ERR_VERIFY;
    }

    resume_state_clear();
    return STM_OK;
}

stm_err_t stm_ota_request_reboot(void)
{
    // 写入备份寄存器 magic 并复位。
    // STM32F4 备份寄存器访问：先开启 PWR 和备份域访问。
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    WRITE_REG(RTC->BKP0R, STM_OTA_FLAG_MAGIC);
    HAL_PWR_DisableBkUpAccess();

    NVIC_SystemReset();
    return STM_OK;     // 实际不会返回
}
