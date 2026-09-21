# stm_ota

STM32 OTA 库：通过 ESP-AT 原始 TCP 和 HTTP Range 同步下载固件，写入 STM32F4
片内 Flash。库不创建任务、不调用 RTOS API、不使用动态内存；裸机和 FreeRTOS
应用使用同一套下载实现。

完整下载前可调用 `stm_ota_probe()`，通过 8 字节 Range 请求读取服务端响应头中的
版本、目标槽、包大小和整包 CRC。应用可在擦除 Flash 前拒绝同版本、低版本或槽位
不匹配的包。

下载进度保存在 STM32F407 的 RTC 备份寄存器 `BKP3R..BKP10R`。每个分片完成 Flash
写入和读回 CRC 后更新下一偏移及前缀 CRC；复位后只有目标地址、包大小、整包 CRC、
分片大小、状态校验和及 Flash 前缀 CRC 全部匹配，才从断点继续，否则自动整槽重下。
长下载中的单次 TCP/CIPSEND/HTTP Range 失败会关闭旧连接并重连，当前分片最多尝试
3 次；连续失败后仍返回错误，并清理本次下载状态。

## 统一错误码与依赖

操作返回 `stm_common/stm_err.h` 中的 `stm_err_t`：`STM_OK=0`，失败码为正数，
必须使用 `rc != STM_OK`，不能使用 `rc < 0`。字节数、状态枚举和 CRC 返回值不是错误码。
不再提供旧的 `stm_ota_err_t` / `STM_OTA_OK` 别名。

| 情况 | 返回码 |
| --- | --- |
| 参数/对齐无效 | `STM_ERR_INVALID_ARG` |
| 未初始化、忙或生命周期不允许 | `STM_ERR_INVALID_STATE` |
| 同步接口从事件回调重入（ESP-AT） | `STM_ERR_INVALID_CONTEXT` |
| 超时 / 传输失败 | `STM_ERR_TIMEOUT` / `STM_ERR_IO` |
| 静态缓冲不足 | `STM_ERR_OUT_OF_RANGE`，不是分配失败 |
| Flash/CRC 校验不一致（OTA） | `STM_ERR_VERIFY` |
| ESP-AT 协议格式错误 / 模块 ERROR、FAIL | `ESP_AT_ERR_PROTO=0x1101` / `ESP_AT_ERR_RESP=0x1102` |

`stm_common` 是无 HAL/RTOS/堆的头文件库。CMake 优先复用已有 target，
其次使用同级 `stm_common/`，最后拉取固定 v1.0.0 commit；
通过 `STM_COMMON_GIT_REPOSITORY` 可配置镜像，通过 `STM_COMMON_FETCH=OFF` 禁止下载。
离线构建请提前提供 target 或同级源码；手动集成只需把其目录加入 include path，不能复制错误码。

这是接口兼容性变更：自定义平台 write 回调也须返回 `stm_err_t`（全量发送成功为
`STM_OK`，短写为 `STM_ERR_IO`，超时为 `STM_ERR_TIMEOUT`），不再返回长度。
最新软件门禁与硬件结果分开记录；完成硬件回归后再按组件发布规范选择新版本，不移动已有 tag。

## 边界

- **只调 `esp_at_client` 公开 API**（`esp_at_tcp.h`）
- 不直接碰 ringbuffer / `esp_at_internal.h`
- 内部用 STM32F4 HAL 写 flash（限 STM32F4 系列）
- `stm_ota_probe()` 和 `stm_ota_download()` 都是同步阻塞调用，应从主循环或唯一的
  OTA 任务调用，不能从 ESP-AT 事件回调调用

## 接入

工程根 CMakeLists 加：

```cmake
add_subdirectory(Lib/stm_ota)
target_link_libraries(your_app PRIVATE stm_ota)
```

依赖：`esp_at_client`（轮询 TCP/HTTP）、`stm32cubemx`（HAL）及 `stm_common`（公共错误码）；`stm_log` 为统一日志组件（无 HAL 的新核心）。
应用另行绑定 ESP-AT 平台，例如 `esp_at_stm32_init()`。ESP-AT 核心共享 stm_log，但不再传递 HAL 依赖。
本次不改变 OTA 的 F407 Flash/BKP 实现，不代表 OTA 已兼容其他 MCU。
不需要链接 FreeRTOS。当前源码仍通过相对路径引用应用的
`common/boot_state_protocol.h`；并入总库前还需单独处理这一板级布局依赖。
日志复用已有 stm_log target 或同级源码；应用配置输出回调和时间戳，
`STM_LOG_ENABLED` 控制 AT/OTA 的统一日志开关。

## 最小示例

```c
#include "stm_ota.h"
#include "stm_log.h"

static void on_progress(uint32_t done, uint32_t total, void *user) {
    (void)user;
    LOGI("ota", "%lu / %lu", (unsigned long)done, (unsigned long)total);
}

stm_ota_config_t cfg = {
    .url            = "http://192.168.4.3/firmware.bin",
    .download_addr  = 0x08040000,           // 当前运行 A 时，目标为 B 槽
    .total_size     = 224 * 1024,           // 本工程固定布局 OTA 包
    .crc32_expected = 0x12345678,           // 服务端算的 CRC32
    .chunk_size     = 1024,
    .progress_cb    = on_progress,
};

stm_ota_image_info_t remote = {0};
stm_err_t r = stm_ota_probe(cfg.url, &remote);
if (r == STM_OK) {
    cfg.total_size = remote.package_size;
    cfg.crc32_expected = remote.package_crc32;
    r = stm_ota_download(&cfg);
}
/* 下载成功后由应用写入本项目的 pending_slot，再执行 NVIC_SystemReset()。 */
```

完整的 A/B 版本门禁、进度、pending 适配及裸机/FreeRTOS 调用方式见
[example/README.md](example/README.md)。

## 服务端协议

STM32 使用标准 `Range: bytes=N-M` 请求分块读取固件。服务端返回 `206 Partial Content`，
并提供 `Content-Range`、`X-CRC32`、`X-Firmware-Version-Code`、`X-Firmware-Slot` 和
`X-Firmware-Size` 响应头。配套服务端位于主工程 `tools/ota_server/ota_server.py`。

## 已知坑

- **ESP-AT `AT+HTTPCLIENT` 不支持自定义 `Range` header**，所以本库通过原始 TCP 自行发送 HTTP/1.1 请求。
- **限定 STM32F4 系列**（HAL 直调），其他 STM32 系列要改 `flash_*` 实现。

## 目录

```
stm_ota/
├── CMakeLists.txt
├── README.md
├── ADMIN.md
├── example/
│   ├── README.md
│   └── main.c
├── inc/
│   └── stm_ota.h
└── src/
    └── stm_ota.c
```

维护架构、Flash/断点约束、故障注入和发布门禁见 [ADMIN.md](ADMIN.md)。
