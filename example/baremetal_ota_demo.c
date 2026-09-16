/**
 * @file    baremetal_ota_demo.c
 * @brief   裸机主循环调用 A/B OTA 参考流程
 */

#include "ota_ab_demo.h"

#include "esp_at_client.h"

static ota_ab_demo_t s_ota_demo;

/** 外设、ESP-AT、Wi-Fi 和 MQTT 已初始化后调用。 */
bool app_baremetal_ota_demo_init(const ota_ab_demo_config_t *config)
{
    return ota_ab_demo_init(&s_ota_demo, config);
}

/** MQTT 回调识别到 update 后调用；本函数不会在回调中下载。 */
void app_baremetal_ota_demo_request(void)
{
    ota_ab_demo_request(&s_ota_demo);
}

/**
 * 放入裸机 while (1)。真实应用还应在此处理看门狗和其他非阻塞状态机。
 */
void app_baremetal_ota_demo_process(void)
{
    esp_at_poll();
    (void)ota_ab_demo_process(&s_ota_demo);
}
