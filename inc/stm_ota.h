/**
 * @file    stm_ota.h
 * @brief   STM32 OTA 库：通过 ESP32 HTTP GET 拉固件，写本地 Flash
 */

#ifndef STM_OTA_H
#define STM_OTA_H

#include <stdint.h>
#include <stddef.h>
#include "stm_err.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef void (*stm_ota_progress_cb_t)(uint32_t done, uint32_t total, void *user);

typedef struct {
    uint32_t image_version;           // 远端镜像版本（MAJOR.MINOR.PATCH 打包值）。
    uint32_t package_size;            // 完整 OTA 包大小
    uint32_t package_crc32;           // 完整 OTA 包 CRC32
    uint8_t  target_slot;             // 目标槽：0=A，1=B
} stm_ota_image_info_t;

typedef struct {
    const char *url;                  // 必填，HTTP URL（服务端需支持 Range）。
    /*
     * STM32F407 A/B 工程中必须填写槽位基址（0x08008000 或 0x08040000）。
     * 0 会解析为旧的默认暂存地址，但该地址不在片内 Flash，最终会
     * 被严格校验拒绝；这样可以保留旧的 0 值语义，同时避免误擦写。
     */
    uint32_t    download_addr;
    /* 必须大于 0，且完整下载范围不能越过目标槽的 OTA 包上限。 */
    uint32_t    total_size;
    /* 0 表示从首个 HTTP Range 响应的 X-CRC32 读取完整包 CRC。 */
    uint32_t    crc32_expected;
    uint32_t    chunk_size;            // 0=默认1024；否则4～1400且4字节对齐。
    stm_ota_progress_cb_t progress_cb;
    void       *progress_user;
} stm_ota_config_t;

/**
 * @brief 读取远端镜像信息，不擦除或写入 Flash
 *
 * 通过一个极小的 HTTP Range 请求读取服务端响应头，供应用在完整下载前完成
 * 版本、槽位和包大小检查。
 * @return STM_OK 成功；STM_ERR_INVALID_ARG 参数无效；STM_ERR_IO 网络/响应失败。
 */
stm_err_t stm_ota_probe(const char *url, stm_ota_image_info_t *info);

/**
 * @brief 通过 HTTP 拉取固件分片，写入 STM32 Flash 并进行 CRC 校验。
 *
 * 阻塞式（通常几十秒到几分钟）。失败立即返回错误码。
 * @return STM_OK 成功；INVALID_ARG 参数无效；IO 网络/Flash 失败；VERIFY CRC 不一致。
 * @note 返回码为 stm_err_t 正数，不可用 <0 判断。只允许单调用者；不可从 AT 回调调用。
 */
stm_err_t stm_ota_download(const stm_ota_config_t *cfg);

/**
 * @brief 写入旧版通用 Bootloader 标志并调用 NVIC_SystemReset
 *
 * 这是兼容旧工程的接口，只写入旧版 BKP0R magic。A/B 工程必须由应用先按
 * 自己的 Bootloader 状态协议写入 pending 槽，不能用本接口代替 pending 请求。
 * 写完后不返回（reset）。
 */
stm_err_t stm_ota_request_reboot(void);

/**
 * @brief CRC-32 / IEEE 多项式 0xEDB88320。
 */
uint32_t stm_ota_crc32(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* STM_OTA_H */
