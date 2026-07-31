/**
 * @file    stm_ota.c
 * @brief   STM32 OTA：通过 ESP32 HTTP GET 拉固件 chunk 写本地 flash
 */

#include "stm_ota.h"

#include "esp_at_client.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "stm32f4xx_hal.h"             // 内部 HAL：stm_ota 限定 STM32F4
#include "FreeRTOS.h"
#include "semphr.h"

// 默认下载区与 chunk 大小
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

// 用绝对地址算 sector（STM32F4 4KB-sector 起点表）
static uint32_t addr_to_sector(uint32_t addr)
{
    if (addr < 0x08004000U) return FLASH_SECTOR_0;
    if (addr < 0x08008000U) return FLASH_SECTOR_1;
    if (addr < 0x0800C000U) return FLASH_SECTOR_2;
    if (addr < 0x08010000U) return FLASH_SECTOR_3;
    if (addr < 0x08020000U) return FLASH_SECTOR_4;
    if (addr < 0x08040000U) return FLASH_SECTOR_5;
    if (addr < 0x08080000U) return FLASH_SECTOR_6;
    if (addr < 0x080C0000U) return FLASH_SECTOR_7;
    if (addr < 0x08100000U) return FLASH_SECTOR_8;
    if (addr < 0x08140000U) return FLASH_SECTOR_9;
    if (addr < 0x08180000U) return FLASH_SECTOR_10;
    return FLASH_SECTOR_11;
}

static int flash_erase(uint32_t addr, uint32_t size)
{
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
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t word = *(const uint32_t *)(data + i);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, word) != HAL_OK) {
            return -1;
        }
    }
    return 0;
}

static int flash_read(uint32_t addr, uint8_t *data, uint32_t len)
{
    memcpy(data, (const void *)addr, len);
    return 0;
}

// CRC32 / IEEE 802.3 polynomial 0xEDB88320
uint32_t stm_ota_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint32_t k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320U & -(crc & 1));
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

// HTTP 拉 1 个 chunk：拼 URL 加 ?offset=N&len=M，返回字节数（<=len）
static int http_pull_chunk(const char *base_url, uint32_t offset, uint32_t want,
                            uint8_t *out, uint32_t *got)
{
    char url[256];
    snprintf(url, sizeof url, "%s?offset=%u&len=%u", base_url, offset, want);
    esp_at_http_resp_t resp = {0};
    if (esp_at_http_get(url, &resp, STM_OTA_HTTP_TIMEOUT_MS) != ESP_AT_OK) {
        if (resp.body) vPortFree(resp.body);
        return -1;
    }
    if (resp.body_len > 0) {
        memcpy(out, resp.body, resp.body_len);
    }
    *got = resp.body_len;
    vPortFree(resp.body);
    return 0;
}

stm_ota_err_t stm_ota_download(const stm_ota_config_t *cfg)
{
    if (!cfg || !cfg->url) return STM_OTA_ERR_INVALID;
    uint32_t addr = cfg->download_addr ? cfg->download_addr : STM_OTA_DEFAULT_DOWNLOAD_ADDR;
    uint32_t chunk = cfg->chunk_size ? cfg->chunk_size : STM_OTA_DEFAULT_CHUNK;
    uint32_t total = cfg->total_size;

    if (flash_unlock() != 0) return STM_OTA_ERR_FLASH;
    if (flash_erase(addr, total) != 0) {
        flash_lock();
        return STM_OTA_ERR_FLASH;
    }

    uint32_t done = 0;
    uint8_t *buf = (uint8_t *)pvPortMalloc(chunk);
    if (!buf) {
        flash_lock();
        return STM_OTA_ERR_INVALID;
    }

    while (done < total) {
        uint32_t want = total - done;
        if (want > chunk) want = chunk;

        uint32_t got = 0;
        if (http_pull_chunk(cfg->url, done, want, buf, &got) != 0) {
            vPortFree(buf);
            flash_lock();
            return STM_OTA_ERR_HTTP;
        }
        if (got == 0) break;          // 服务端断流

        if (flash_write(addr + done, buf, got) != 0) {
            vPortFree(buf);
            flash_lock();
            return STM_OTA_ERR_FLASH;
        }
        done += got;

        if (cfg->progress_cb) {
            cfg->progress_cb(done, total, cfg->progress_user);
        }
    }
    vPortFree(buf);
    flash_lock();

    if (done != total) return STM_OTA_ERR_HTTP;

    if (cfg->crc32_expected) {
        uint32_t crc = stm_ota_crc32((const uint8_t *)addr, total);
        if (crc != cfg->crc32_expected) return STM_OTA_ERR_CRC;
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
