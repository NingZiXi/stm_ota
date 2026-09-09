/**
 * @file    stm_ota.h
 * @brief   STM32 OTA 库：通过 ESP32 HTTP GET 拉固件，写本地 Flash
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
    /*
     * STM32F407 A/B 工程中必须填写槽位基址（0x08008000 或 0x08040000）。
     * 0 会解析为旧的默认 staging 地址，但该地址不在片内 Flash，最终会
     * 被严格校验拒绝；这样可以保留旧的 0 值语义，同时避免误擦写。
     */
    uint32_t    download_addr;
    /* 必须大于 0，且完整下载范围不能越过目标槽的 OTA 包上限。 */
    uint32_t    total_size;
    /* 0 表示从首个 HTTP Range 响应的 X-CRC32 读取整包 CRC。 */
    uint32_t    crc32_expected;
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
