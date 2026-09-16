/**
 * @file    freertos_ota_demo.c
 * @brief   FreeRTOS/CMSIS-RTOS 单任务调用 A/B OTA 参考流程
 *
 * RTOS 依赖只存在于示例应用，不进入 stm_ota 库。
 */

#include "ota_ab_demo.h"

#include "esp_at_client.h"

#include "cmsis_os2.h"

static ota_ab_demo_t s_ota_demo;

bool app_freertos_ota_demo_init(const ota_ab_demo_config_t *config)
{
    return ota_ab_demo_init(&s_ota_demo, config);
}

/** MQTT 回调只调用本函数置标志。 */
void app_freertos_ota_demo_request(void)
{
    ota_ab_demo_request(&s_ota_demo);
}

/** 所有 ESP-AT 同步 API 和 OTA 下载都集中在这一个任务。 */
void app_freertos_ota_task(void *argument)
{
    (void)argument;
    for (;;) {
        esp_at_poll();
        (void)ota_ab_demo_process(&s_ota_demo);
        osDelay(1U);
    }
}
