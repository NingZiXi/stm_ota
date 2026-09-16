/**
 * @file    ota_ab_demo.h
 * @brief   与具体 Bootloader 状态协议解耦的 A/B OTA 参考流程
 */

#ifndef STM_OTA_AB_DEMO_H
#define STM_OTA_AB_DEMO_H

#include "stm_ota.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_AB_DEMO_SLOT_A     0U
#define OTA_AB_DEMO_SLOT_B     1U
#define OTA_AB_DEMO_SLOT_NONE  0xFFU

typedef struct {
    uint8_t (*get_running_slot)(void *user);
    bool (*read_image_version)(uint8_t slot, uint32_t *version, void *user);
    bool (*request_pending_slot)(uint8_t slot, void *user);
    void (*system_reset)(void *user);
    void *user;
} ota_ab_demo_boot_ops_t;

typedef struct {
    const char *slot_url[2];
    uint32_t slot_base[2];
    uint32_t slot_capacity;
    uint32_t chunk_size;
    bool allow_downgrade;
    stm_ota_progress_cb_t progress_cb;
    void *progress_user;
    ota_ab_demo_boot_ops_t boot;
} ota_ab_demo_config_t;

typedef struct {
    const ota_ab_demo_config_t *config;
    volatile bool requested;
    bool running;
} ota_ab_demo_t;

/** 初始化示例状态；配置对象必须在整个运行期保持有效。 */
bool ota_ab_demo_init(ota_ab_demo_t *demo,
                      const ota_ab_demo_config_t *config);

/** 可从 MQTT 事件回调调用：只记录请求，不执行同步下载。 */
void ota_ab_demo_request(ota_ab_demo_t *demo);

/** 在主循环或唯一 OTA 任务调用；没有请求时立即返回。 */
stm_ota_err_t ota_ab_demo_process(ota_ab_demo_t *demo);

#ifdef __cplusplus
}
#endif

#endif /* STM_OTA_AB_DEMO_H */
