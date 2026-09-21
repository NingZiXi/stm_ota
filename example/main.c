/** @file main.c
 *  @brief STM32F407 A/B OTA 单文件示例：只下载到非运行槽。
 */
#include "stm_ota.h"
#include "esp_at_client.h"
#include "esp_at_port_stm32.h"
#include "esp_at_wifi.h"
#include "esp_at_mqtt.h"
#include "esp_at_tcp.h"
#include <stdio.h>
#include "stm_log.h"
#include <string.h>

// 以下地址仅适用于本示例的 F407 A/B 布局；必须与链接脚本和 Bootloader 一致。
#define SLOT_A_BASE 0x08008000U
#define SLOT_B_BASE 0x08040000U
#define SLOT_CAPACITY (224U * 1024U)
#define CONTROL_TOPIC "stm32/example/ota/control"
static const char *const urls[2] = {
    "http://192.168.1.100:8081/firmware-A.ota.bin",
    "http://192.168.1.100:8081/firmware-B.ota.bin",
};
static const uint32_t bases[2] = {SLOT_A_BASE, SLOT_B_BASE};

// 应用必须提供这些板级函数，不提供空实现来假装成功。
// board_init 初始化 HAL/时钟/中断/UART/DMA/stm_log，填写有效端口配置。
void board_init(esp_at_port_config_t *port);
// 校验 metadata/CRC，生产配置还须验签；成功后返回版本。
stm_err_t board_read_verified_version(uint8_t slot, uint32_t *version);
// 按 Bootloader 协议写入并读回 pending，不能修改 active。
stm_err_t board_request_pending(uint8_t slot);
// 只有健康检查成功才确认当前槽；实现必须校验当前运行槽与试启动状态。
stm_err_t board_confirm_boot(uint8_t slot);

static bool requested;
static uint8_t running_slot;
static uint32_t running_version;

static void on_message(const esp_at_event_payload_t *event, void *user)
{
    (void)user;
    if (event->type == ESP_AT_EVENT_MQTT_MESSAGE
        && event->topic && event->topic_len == sizeof(CONTROL_TOPIC) - 1U
        && memcmp(event->topic, CONTROL_TOPIC, sizeof(CONTROL_TOPIC) - 1U) == 0
        && event->data && event->data_len == 6U
        && memcmp(event->data, "update", 6U) == 0) {
        requested = true; // 回调只置标志，绝不重入同步下载。
    }
}

static void progress(uint32_t done, uint32_t total, void *user)
{
    (void)user;
    LOGI("example", "OTA %lu/%lu", (unsigned long)done, (unsigned long)total);
}

static stm_err_t update(void)
{
    const uint8_t target = running_slot ^ 1U;
    stm_ota_image_info_t remote = {0};
    stm_err_t rc = stm_ota_probe(urls[target], &remote);
    if (rc != STM_OK) return rc;
    if (remote.target_slot != target || !remote.package_size
        || remote.package_size > SLOT_CAPACITY) return STM_ERR_INVALID_ARG;
    if (remote.image_version <= running_version) return STM_ERR_INVALID_STATE;
    // 危险操作：下载会擦除目标槽。地址来自已验证的 running_slot 的另一槽。
    const stm_ota_config_t config = {
        .url = urls[target], .download_addr = bases[target],
        .total_size = remote.package_size, .crc32_expected = remote.package_crc32,
        .chunk_size = 1024U, .progress_cb = progress,
    };
    rc = stm_ota_download(&config);
    if (rc != STM_OK) return rc;
    uint32_t version = 0;
    rc = board_read_verified_version(target, &version);
    if (rc != STM_OK) return rc;
    if (version != remote.image_version) return STM_ERR_VERIFY;
    rc = board_request_pending(target);
    if (rc != STM_OK) return rc;
    NVIC_SystemReset();
    return STM_ERR_IO; // 复位不应返回。
}

int main(void)
{
    esp_at_port_config_t port = {0};
    board_init(&port);
    stm_err_t rc = STM_ERR_INVALID_STATE;
    if (SCB->VTOR == SLOT_A_BASE || SCB->VTOR == SLOT_B_BASE) {
        running_slot = SCB->VTOR == SLOT_A_BASE ? 0U : 1U;
        rc = board_read_verified_version(running_slot, &running_version);
    }
    if (rc == STM_OK) rc = esp_at_stm32_init(&port);
    if (rc == STM_OK) rc = esp_at_wifi_init(1U);
    if (rc == STM_OK) {
        esp_at_wifi_query_t state = {0};
        rc = esp_at_wifi_query_state(&state, 3000U);
        if (rc == STM_OK && state.state != ESP_AT_WIFI_GOT_IP)
            rc = esp_at_wifi_connect("YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD", 30000U);
    }
    if (rc == STM_OK) rc = esp_at_tcp_init();
    const esp_at_mqtt_user_cfg_t mqtt = {
        .link_id = 0U, .scheme = 1U, .client_id = "stm32-ota-example",
        .keepalive_s = 60U,
    };
    const esp_at_mqtt_conn_cfg_t connection = {
        .link_id = 0U, .timeout_s = 60U, .path = "",
    };
    if (rc == STM_OK)
        rc = esp_at_register_event_cb(ESP_AT_EVENT_MQTT_MESSAGE, on_message, NULL);
    if (rc == STM_OK)
        rc = esp_at_mqtt_connect(&mqtt, &connection, "broker.emqx.io", 1883U, 15000U);
    if (rc == STM_OK)
        rc = esp_at_mqtt_subscribe(0U, CONTROL_TOPIC, 1U, NULL, NULL, 5000U);
    if (rc == STM_OK) {
        uint32_t started = HAL_GetTick();
        while ((uint32_t)(HAL_GetTick() - started) < 5000U) {
            esp_at_poll();
            HAL_Delay(1U);
        }
        // 示例以网络初始化成功和 5 秒存活作为健康条件；产品应补充业务自检。
        rc = board_confirm_boot(running_slot);
    }
    LOGI("example", "OTA init rc=0x%04lX", (unsigned long)rc);
    for (;;) {
        esp_at_poll();
        if (rc == STM_OK && requested) {
            requested = false;
            stm_err_t result = update();
            LOGI("example", "OTA result=0x%04lX", (unsigned long)result);
        }
        HAL_Delay(1U);
    }
}
