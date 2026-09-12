# stm_ota

STM32 OTA 库：通过 ESP32 HTTP GET 拉固件，写本地 flash。

完整下载前可调用 `stm_ota_probe()`，通过 8 字节 Range 请求读取服务端响应头中的
版本、目标槽、包大小和整包 CRC。应用可在擦除 Flash 前拒绝同版本、低版本或槽位
不匹配的包。

下载进度保存在 STM32F407 的 RTC 备份寄存器 `BKP3R..BKP10R`。每个分片完成 Flash
写入和读回 CRC 后更新下一偏移及前缀 CRC；复位后只有目标地址、包大小、整包 CRC、
分片大小、状态校验和及 Flash 前缀 CRC 全部匹配，才从断点继续，否则自动整槽重下。

## 边界

- **只调 `esp_at_client` 公开 API**（`esp_at_tcp.h`）
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
    .chunk_size     = 1024,
    .progress_cb    = on_progress,
};

stm_ota_image_info_t remote = {0};
stm_ota_err_t r = stm_ota_probe(cfg.url, &remote);
if (r == STM_OTA_OK) {
    cfg.total_size = remote.package_size;
    cfg.crc32_expected = remote.package_crc32;
    r = stm_ota_download(&cfg);
}
if (r == STM_OTA_OK) {
    stm_ota_request_reboot();    // 写 flag + NVIC_SystemReset
}
```

## 服务端协议

STM32 使用标准 `Range: bytes=N-M` 请求分块读取固件。服务端返回 `206 Partial Content`，
并提供 `Content-Range`、`X-CRC32`、`X-Firmware-Version-Code`、`X-Firmware-Slot` 和
`X-Firmware-Size` 响应头。配套服务端位于主工程 `tools/ota_server/ota_server.py`。

## 已知坑

- **ESP-AT `AT+HTTPCLIENT` 不支持自定义 `Range` header**，所以本库通过 raw TCP 自行发送 HTTP/1.1 请求。
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
