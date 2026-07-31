# stm_ota

STM32 OTA 库：通过 ESP32 HTTP GET 拉固件，写本地 flash。

## 边界

- **只调 `esp_at_client` 公开 API**（`esp_at_http_get`）
- 不直接碰 ringbuffer / `esp_at_internal.h`
- 内部用 STM32F4 HAL 写 flash（限 STM32F4 系列）

## 接入

工程根 CMakeLists 加：

```cmake
add_subdirectory(Lib/stm_ota)
target_link_libraries(your_app PRIVATE stm_ota)
```

依赖：`esp_at_client` target（提供 HTTP 拉固件）、`stm32cubemx` target（HAL）。

## 最小示例

```c
#include "stm_ota.h"

static void on_progress(uint32_t done, uint32_t total, void *user) {
    LOGI("ota", "%lu / %lu", (unsigned long)done, (unsigned long)total);
}

stm_ota_config_t cfg = {
    .url            = "http://192.168.4.3/firmware.bin",
    .download_addr  = 0x08080000,           // App 之后的 download 区
    .total_size     = 256 * 1024,           // 固件大小
    .crc32_expected = 0x12345678,           // 服务端算的 CRC32
    .chunk_size     = 512,
    .progress_cb    = on_progress,
};

stm_ota_err_t r = stm_ota_download(&cfg);
if (r == STM_OTA_OK) {
    stm_ota_request_reboot();    // 写 flag + NVIC_SystemReset
}
```

## 服务端协议

STM32 发起 `GET <url>?offset=N&len=M`，服务端返回 `[offset, offset+M)` 字节的 raw 固件数据。

服务端例子（Python）：见主项目 `D:\hs_project\stm_ota_server\stm_ota_server.py`（待开发）。

## 已知坑

- **ESP-AT `AT+HTTPCLIENT` 不支持 `Range` header**，所以 STM32 端用 URL `?offset=N&len=M` query 参数把 offset 传给服务端。
- **bootloader 入口**：`stm_ota_request_reboot` 写备份寄存器 magic + 复位，**bootloader 还没实现**（下一步）。
- **限定 STM32F4 系列**（HAL 直调），其他 STM32 系列要改 `flash_*` 实现。

## 目录

```
stm_ota/
├── CMakeLists.txt
├── README.md
├── inc/
│   └── stm_ota.h
└── src/
    └── stm_ota.c
```
