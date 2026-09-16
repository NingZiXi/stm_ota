# stm_ota 示例程序

本目录提供完整的 A/B OTA 业务参考流程，并把项目特有的 Bootloader 状态协议隔离为四个回调。
这样 `stm_ota` 库继续只负责下载、Flash 和 CRC，不与某个 pending 编码耦合。

## 1. 文件

| 文件 | 作用 |
| --- | --- |
| `ota_ab_demo.h/.c` | probe、版本门禁、下载、metadata 复核、pending 和复位 |
| `baremetal_ota_demo.c` | 裸机 `while (1)` 包装 |
| `freertos_ota_demo.c` | CMSIS-RTOS 单任务包装，RTOS 仅存在于应用层 |

## 2. 必须提供的 Bootloader 适配

应用必须实现以下能力并填入 `ota_ab_demo_boot_ops_t`：

```c
static uint8_t app_get_running_slot(void *user)
{
    (void)user;
    return boot_state_get_running_slot();       /* 通常根据 SCB->VTOR */
}

static bool app_read_version(uint8_t slot, uint32_t *version, void *user)
{
    (void)user;
    return app_read_image_version(slot, version); /* 读取并校验 metadata */
}

static bool app_request_pending(uint8_t slot, void *user)
{
    (void)user;
    return boot_state_request_slot(slot);       /* 写入后必须读回确认 */
}

static void app_reset(void *user)
{
    (void)user;
    NVIC_SystemReset();
}
```

上面的函数名来自本项目，其他工程应替换为自己的 Bootloader 适配。不要在库内复制另一份状态协议。

## 3. 创建 A/B 配置

```c
static void on_progress(uint32_t done, uint32_t total, void *user)
{
    (void)user;
    LOGI("ota", "%lu/%lu", (unsigned long)done, (unsigned long)total);
}

static const ota_ab_demo_config_t s_ota_config = {
    .slot_url = {
        "http://192.168.1.100:8081/firmware-A.ota.bin",
        "http://192.168.1.100:8081/firmware-B.ota.bin",
    },
    .slot_base = {0x08008000U, 0x08040000U},
    .slot_capacity = 224U * 1024U,
    .chunk_size = 1024U,
    .allow_downgrade = false,
    .progress_cb = on_progress,
    .progress_user = NULL,
    .boot = {
        .get_running_slot = app_get_running_slot,
        .read_image_version = app_read_version,
        .request_pending_slot = app_request_pending,
        .system_reset = app_reset,
        .user = NULL,
    },
};
```

地址和容量必须来自项目唯一的 Flash 布局定义，不能在多个模块中各维护一份。

## 4. MQTT 回调

事件回调只记录请求：

```c
static void on_mqtt_message(const esp_at_event_payload_t *event, void *user)
{
    (void)user;
    if (event->type == ESP_AT_EVENT_MQTT_MESSAGE
        && event->data_len == 6U
        && memcmp(event->data, "update", 6U) == 0) {
        app_baremetal_ota_demo_request();
    }
}
```

不能在 MQTT 回调内直接调用 `stm_ota_probe()` 或 `stm_ota_download()`，否则会重入 ESP-AT
同步命令状态机。

## 5. 裸机用法

```c
/* Wi-Fi、CIPMUX、MQTT 连接和订阅成功后： */
if (!app_baremetal_ota_demo_init(&s_ota_config)) {
    Error_Handler();
}

for (;;) {
    app_baremetal_ota_demo_process();
    HAL_Delay(1U);
}
```

## 6. FreeRTOS 用法

ESP-AT 初始化时注入 `osDelay()` 包装，之后只创建一个拥有 ESP-AT/OTA 的任务：

```c
if (!app_freertos_ota_demo_init(&s_ota_config)) {
    Error_Handler();
}

osThreadNew(app_freertos_ota_task, NULL, &ota_task_attributes);
```

其他任务不得并发调用 ESP-AT 或 OTA 同步 API。需要触发更新时只调用
`app_freertos_ota_demo_request()`。

## 7. 加入构建

裸机：

```cmake
target_sources(your_app PRIVATE
    Lib/stm_ota/example/ota_ab_demo.c
    Lib/stm_ota/example/baremetal_ota_demo.c
)
target_include_directories(your_app PRIVATE Lib/stm_ota/example)
target_link_libraries(your_app PRIVATE stm_ota)
```

FreeRTOS 将 `baremetal_ota_demo.c` 换成 `freertos_ota_demo.c`。示例不会自动加入库构建，避免把
应用策略或 RTOS 依赖带进 `stm_ota`。

## 8. 成功与失败标准

成功必须同时满足：

- probe 槽位与目标槽一致；
- 版本门禁通过；
- 包大小不超过槽容量；
- 接收 CRC、Flash CRC、期望 CRC 一致；
- 下载后 metadata 版本与 probe 一致；
- pending 写入并读回成功；
- Bootloader 校验通过并试启动；
- 新 App 健康确认后 active 更新且 pending 清除。

任一步失败都不得写 pending，旧 active 槽应继续可启动。
