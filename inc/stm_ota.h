/**
 * @file    stm_ota.h
 * @brief   STM32 OTA 库：通过 ESP32 HTTP GET 拉固件，写本地 flash
 */

#ifndef STM_OTA_H
#define STM_OTA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STM_OTA_OK            = 0,
    STM_OTA_ERR_INVALID   = -1,
    STM_OTA_ERR_HTTP       = -2,
    STM_OTA_ERR_FLASH      = -3,
    STM_OTA_ERR_CRC        = -4,
    STM_OTA_ERR_TIMEOUT    = -5,
} stm_ota_err_t;

typedef void (*stm_ota_progress_cb_t)(uint32_t done, uint32_t total, void *user);

typedef struct {
    const char *url;                  // 必填，HTTP URL（服务端需支持 Range）
    uint32_t    download_addr;         // STM32 flash 起始地址（默认 0x08080000）
    uint32_t    total_size;            // 0 = HEAD 探查
    uint32_t    crc32_expected;        // 0 = 不校验
    uint32_t    chunk_size;            // 默认 512，<= 1400（AT_RESP_TEXT_MAX 推荐）
    stm_ota_progress_cb_t progress_cb;
    void       *progress_user;
} stm_ota_config_t;

/**
 * @brief HTTP 拉固件 chunk → 写 STM32 flash → CRC 校验
 *
 * 阻塞式（通常几十秒到几分钟）。失败立即返回错误码。
 */
stm_ota_err_t stm_ota_download(const stm_ota_config_t *cfg);

/**
 * @brief 写 bootloader 标志 + NVIC_SystemReset
 *
 * 写完后不返回（reset）。
 */
stm_ota_err_t stm_ota_request_reboot(void);

/**
 * @brief CRC-32 / IEEE polynomial 0xEDB88320
 */
uint32_t stm_ota_crc32(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* STM_OTA_H */
