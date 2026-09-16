/**
 * @file    ota_ab_demo.c
 * @brief   A/B OTA 的探测、版本门禁、下载、pending 和复位参考实现
 */

#include "ota_ab_demo.h"

#include "stm_log.h"

#include <stddef.h>

static const char *TAG = "ota_ab_example";

static bool config_is_valid(const ota_ab_demo_config_t *config)
{
    return config != NULL
        && config->slot_url[OTA_AB_DEMO_SLOT_A] != NULL
        && config->slot_url[OTA_AB_DEMO_SLOT_B] != NULL
        && config->slot_base[OTA_AB_DEMO_SLOT_A] != 0U
        && config->slot_base[OTA_AB_DEMO_SLOT_B] != 0U
        && config->slot_capacity > 0U
        && config->boot.get_running_slot != NULL
        && config->boot.read_image_version != NULL
        && config->boot.request_pending_slot != NULL
        && config->boot.system_reset != NULL;
}

bool ota_ab_demo_init(ota_ab_demo_t *demo,
                      const ota_ab_demo_config_t *config)
{
    if (demo == NULL || !config_is_valid(config)) {
        return false;
    }
    demo->config = config;
    demo->requested = false;
    demo->running = false;
    return true;
}

void ota_ab_demo_request(ota_ab_demo_t *demo)
{
    if (demo != NULL) {
        demo->requested = true;
    }
}

stm_ota_err_t ota_ab_demo_process(ota_ab_demo_t *demo)
{
    if (demo == NULL || !config_is_valid(demo->config)) {
        return STM_OTA_ERR_INVALID;
    }
    if (!demo->requested || demo->running) {
        return STM_OTA_OK;
    }

    demo->requested = false;
    demo->running = true;
    const ota_ab_demo_config_t *config = demo->config;
    const uint8_t current = config->boot.get_running_slot(
        config->boot.user);
    if (current > OTA_AB_DEMO_SLOT_B) {
        LOGE(TAG, "cannot identify running slot");
        demo->running = false;
        return STM_OTA_ERR_INVALID;
    }
    const uint8_t target = current == OTA_AB_DEMO_SLOT_A
        ? OTA_AB_DEMO_SLOT_B : OTA_AB_DEMO_SLOT_A;

    uint32_t current_version = 0U;
    if (!config->boot.read_image_version(current, &current_version,
                                         config->boot.user)) {
        LOGE(TAG, "current image version unavailable");
        demo->running = false;
        return STM_OTA_ERR_INVALID;
    }

    stm_ota_image_info_t remote = {0};
    stm_ota_err_t result = stm_ota_probe(config->slot_url[target], &remote);
    if (result != STM_OTA_OK) {
        LOGE(TAG, "preflight failed: %d", (int)result);
        demo->running = false;
        return result;
    }
    if (remote.target_slot != target
        || remote.package_size == 0U
        || remote.package_size > config->slot_capacity) {
        LOGE(TAG, "preflight mismatch slot=%u size=%lu", remote.target_slot,
             (unsigned long)remote.package_size);
        demo->running = false;
        return STM_OTA_ERR_INVALID;
    }
    if (!config->allow_downgrade
        && remote.image_version <= current_version) {
        LOGW(TAG, "version rejected current=0x%08lX remote=0x%08lX",
             (unsigned long)current_version,
             (unsigned long)remote.image_version);
        demo->running = false;
        return STM_OTA_ERR_INVALID;
    }

    const stm_ota_config_t download = {
        .url = config->slot_url[target],
        .download_addr = config->slot_base[target],
        .total_size = remote.package_size,
        .crc32_expected = remote.package_crc32,
        .chunk_size = config->chunk_size,
        .progress_cb = config->progress_cb,
        .progress_user = config->progress_user,
    };
    LOGI(TAG, "download slot %c version=0x%08lX size=%lu",
         target == OTA_AB_DEMO_SLOT_A ? 'A' : 'B',
         (unsigned long)remote.image_version,
         (unsigned long)remote.package_size);
    result = stm_ota_download(&download);
    if (result != STM_OTA_OK) {
        LOGE(TAG, "download failed: %d", (int)result);
        demo->running = false;
        return result;
    }

    uint32_t downloaded_version = 0U;
    if (!config->boot.read_image_version(target, &downloaded_version,
                                         config->boot.user)
        || downloaded_version != remote.image_version) {
        LOGE(TAG, "downloaded metadata version mismatch");
        demo->running = false;
        return STM_OTA_ERR_CRC;
    }
    if (!config->boot.request_pending_slot(target, config->boot.user)) {
        LOGE(TAG, "pending slot write failed");
        demo->running = false;
        return STM_OTA_ERR_FLASH;
    }

    LOGI(TAG, "pending slot %c committed, reset",
         target == OTA_AB_DEMO_SLOT_A ? 'A' : 'B');
    config->boot.system_reset(config->boot.user);
    demo->running = false; /* system_reset 正常不会返回。 */
    return STM_OTA_OK;
}
