/**
 * @file    stm_ota.c
 * @brief   STM32 OTA：通过 ESP32 HTTP GET 拉固件 chunk 写本地 flash
 */

#include "stm_ota.h"

/* A/B 槽边界由应用与 Bootloader 共用，避免 OTA 库再维护一份地址。 */
#include "../../../common/boot_state_protocol.h"

#include "esp_at_client.h"
#include "esp_at_tcp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "stm32f4xx_hal.h"             // 内部 HAL：stm_ota 限定 STM32F4
#include "FreeRTOS.h"
#include "semphr.h"
#include "cmsis_os2.h"

// 兼容旧配置的默认 staging 地址；F407 片内 Flash 范围校验会拒绝它，
// 调用方必须显式传入 BOOT_SLOT_A_BASE 或 BOOT_SLOT_B_BASE。
#define STM_OTA_DEFAULT_DOWNLOAD_ADDR  0x08080000U
#define STM_OTA_DEFAULT_CHUNK          512U
#define STM_OTA_HTTP_TIMEOUT_MS        8000U
#define STM_OTA_FLAG_ADDR              0x40024000U   // 备份寄存器：实际放 (BKP_BASE + 0x04)
#define STM_OTA_FLAG_MAGIC             0x0FADE001U

// flash 工具（HAL 直调，限定 STM32F4）
static int flash_unlock(void)
{
    HAL_FLASH_Unlock();
    return 0;
}

static int flash_lock(void)
{
    HAL_FLASH_Lock();
    return 0;
}

// 按绝对地址计算 STM32F407 Flash sector
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
    if (data == NULL && len != 0U) {
        return -1;
    }
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
 * 返回值为 0 表示合法，并通过 aligned_size 返回按 Flash word 写入所需的
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

    /* flash_write() 按 32-bit word 编程，检查补齐后的最后一个 word。 */
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

// 更新 CRC32 状态
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

// 计算 CRC32
uint32_t stm_ota_crc32(const uint8_t *data, uint32_t len)
{
    return crc32_update(0xFFFFFFFFU, data, len) ^ 0xFFFFFFFFU;
}

// 解析 URL：http://host[:port]/path
static int parse_url(const char *url, char *host, uint16_t host_sz,
                     uint16_t *port, char *path, uint16_t path_sz)
{
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t h_len = (size_t)(colon - p);
        if (h_len >= host_sz) return -1;
        memcpy(host, p, h_len);
        host[h_len] = '\0';
        *port = (uint16_t)atoi(colon + 1);
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

// 文件级 static：stm_ota_download 末尾能 close
static esp_at_tcp_t s_tcp;
static bool s_connected = false;
static char s_host[64];
static char s_path[64];
static uint16_t s_port = 0;

// 找 HTTP 头结束 "\r\n\r\n"，返回头结束偏移（含 \r\n\r\n 长度）
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

// HTTP 拉 1 个 chunk：raw TCP 长连接 + Range 头
// host/port/path/tcp 在首次调用时建连接，后续复用
static int http_pull_chunk(const char *base_url, uint32_t offset, uint32_t want,
                            uint8_t *out, uint32_t *got)
{
    if (!s_connected) {
        if (parse_url(base_url, s_host, sizeof s_host, &s_port, s_path, sizeof s_path) != 0) {
            LOGE("ota", "bad url: %s", base_url);
            return -1;
        }
        if (esp_at_tcp_connect(&s_tcp, s_host, s_port, 8000) != ESP_AT_OK) {
            LOGE("ota", "TCP connect %s:%u failed", s_host, s_port);
            return -1;
        }
        s_connected = true;
    }

    if (esp_at_tcp_http_get_range(&s_tcp, s_host, s_path,
                                    offset, want, STM_OTA_HTTP_TIMEOUT_MS) != ESP_AT_OK) {
        LOGE("ota", "HTTP GET range %lu-%lu failed", offset, offset + want - 1);
        esp_at_tcp_close(&s_tcp);
        s_connected = false;
        return -1;
    }

    // +IPD 帧 = HTTP 响应：状态行 + 头 + \r\n\r\n + body
    static uint8_t s_resp[STM_OTA_DEFAULT_CHUNK * 2];   // 1KB 够放响应头 + 512 字节 body
    uint32_t got_bytes = 0;
    if (esp_at_tcp_recv_body(&s_tcp, s_resp, sizeof s_resp, &got_bytes, 12000) != ESP_AT_OK) {
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

    uint32_t body_off = (uint32_t)hdr_end;
    uint32_t body_len = got_bytes - body_off;
    if (body_len > want) body_len = want;
    memcpy(out, s_resp + body_off, body_len);
    *got = body_len;
    LOGD("ota", "chunk %lu got %u body bytes (hdr_end=%d)", offset, (unsigned)body_len, hdr_end);
    return 0;
}

stm_ota_err_t stm_ota_download(const stm_ota_config_t *cfg)
{
    if (!cfg || !cfg->url || cfg->url[0] == '\0') {
        return STM_OTA_ERR_INVALID;
    }

    uint32_t addr = cfg->download_addr ? cfg->download_addr : STM_OTA_DEFAULT_DOWNLOAD_ADDR;
    uint32_t chunk = cfg->chunk_size ? cfg->chunk_size : STM_OTA_DEFAULT_CHUNK;
    uint32_t total = cfg->total_size;
    static uint8_t s_buf[STM_OTA_DEFAULT_CHUNK];      // 跳过 heap：测试期不依赖 malloc
    uint32_t programmed_size = 0U;

    /* 所有参数先校验，再解锁/擦除 Flash，避免错误配置造成破坏性副作用。 */
    if (validate_download_range(addr, total, &programmed_size) != 0
        || chunk > sizeof s_buf) {
        return STM_OTA_ERR_INVALID;
    }

    if (flash_unlock() != 0) return STM_OTA_ERR_FLASH;
    if (flash_erase(addr, programmed_size) != 0) {
        flash_lock();
        return STM_OTA_ERR_FLASH;
    }

    uint32_t done = 0;
    uint32_t download_crc = 0xFFFFFFFFU;

    while (done < total) {
        uint32_t want = total - done;
        if (want > chunk) want = chunk;

        uint32_t got = 0;
        if (http_pull_chunk(cfg->url, done, want, s_buf, &got) != 0) {
            flash_lock();
            return STM_OTA_ERR_HTTP;
        }
        if (got == 0) break;

        uint32_t chunk_crc = stm_ota_crc32(s_buf, got);
        download_crc = crc32_update(download_crc, s_buf, got);
        if (flash_write(addr + done, s_buf, got) != 0) {
            flash_lock();
            return STM_OTA_ERR_FLASH;
        }
        uint32_t flash_crc = stm_ota_crc32((const uint8_t *)(addr + done), got);
        LOGI("ota", "chunk %lu len=%lu rx_crc=0x%08lX flash_crc=0x%08lX",
             (unsigned long)done, (unsigned long)got,
             (unsigned long)chunk_crc, (unsigned long)flash_crc);
        if (flash_crc != chunk_crc) {
            flash_lock();
            return STM_OTA_ERR_FLASH;
        }
        done += got;

        if (cfg->progress_cb) {
            cfg->progress_cb(done, total, cfg->progress_user);
        }
    }
    flash_lock();

    if (s_connected) {
        esp_at_tcp_close(&s_tcp);
        s_connected = false;
    }

    if (done != total) return STM_OTA_ERR_HTTP;

    if (cfg->crc32_expected) {
        uint32_t rx_crc = download_crc ^ 0xFFFFFFFFU;
        uint32_t flash_crc = stm_ota_crc32((const uint8_t *)addr, total);
        LOGI("ota", "final rx_crc=0x%08lX flash_crc=0x%08lX expected=0x%08lX",
             (unsigned long)rx_crc, (unsigned long)flash_crc,
             (unsigned long)cfg->crc32_expected);
        if (rx_crc != cfg->crc32_expected || flash_crc != cfg->crc32_expected) {
            return STM_OTA_ERR_CRC;
        }
    }

    return STM_OTA_OK;
}

stm_ota_err_t stm_ota_request_reboot(void)
{
    // 写备份寄存器 magic + 复位
    // STM32F4 backup register 访问：先 PWR + 备份访问使能
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    WRITE_REG(RTC->BKP0R, STM_OTA_FLAG_MAGIC);
    HAL_PWR_DisableBkUpAccess();

    NVIC_SystemReset();
    return STM_OTA_OK;     // 实际不会返回
}
