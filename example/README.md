# stm_ota 单文件示例

只保留 `main.c`：从网络初始化、MQTT 请求到版本门禁、下载、pending 和复位顺序阅读。
没有示例头文件、配置回调表或裸机/RTOS 多层包装；日志统一使用 stm_log。

## 使用前必须完成

这是 STM32F407 A/B 布局的参考应用，不是可直接烧录的独立 BSP。
库仍依赖主工程 `common/boot_state_protocol.h` 的布局、F4 HAL、ESP-AT 和 stm_common。

应用需要实现 main.c 声明的四个函数：

| 函数 | 契约 |
| --- | --- |
| `board_init(&port)` | HAL、时钟、GPIO、UART/DMA、NVIC、tick、stm_log；填写有效端口 |
| `board_read_verified_version(slot, &version)` | 校验 metadata/CRC；生产配置验签，成功才返回版本 |
| `board_request_pending(slot)` | 按 Bootloader 协议写入并读回 pending，不修改 active |
| `board_confirm_boot(slot)` | 检查试启动/当前槽，健康成功后确认，不可盲写 active |

这些函数故意不提供“返回成功”的空实现；未接入时链接失败，避免误擦写或绕过校验。
错误返回统一 `stm_err_t`，`STM_OK=0`，其他为正数。

把示例中的地址、容量、URL、Wi-Fi 和 MQTT 配置替换成自己的值。
本示例 A=`0x08008000`、B=`0x08040000`、OTA 包容量 224 KiB；
必须与 Bootloader、链接脚本、包格式完全一致。

## 运行过程

1. 根据 VTOR 识别运行槽并校验当前镜像；未知槽拒绝 OTA。
2. 初始化 ESP-AT、Wi-Fi、CIPMUX、MQTT 和控制主题订阅。
3. 持续轮询 5 秒后确认健康；任何初始化失败都不确认。
4. 向 `stm32/example/ota/control` 发送 `update`；回调只置标志。
5. 主循环 probe 非运行槽，拒绝同版本/降级、错槽和超容量。
6. 下载后校验目标镜像及版本，写入并读回 pending，然后复位。
7. Bootloader 再校验并试启动；新应用健康确认后成为 active。

**下载会擦除非运行槽。** 不能用假槽位或未经校验的 VTOR 试运行。
失败不写 pending、不改 active；CRC 不是签名认证。
公网 MQTT 无认证仅适合实验室；产品须另行设计命令授权。

## 在已有项目中验证

本 OTA 工程保留已有 `app_main()` 和板级入口，临时把示例流程复制进现有业务文件，
按已有 Bootloader API 实现四个板级函数，不新增示例预设。
新建工程则把本文件作为唯一 main，另行提供 BSP 和启动文件。

FreeRTOS 使用同样流程放入一个任务，端口 delay_ms 注入 osDelay 包装；
不要从另一个任务或 MQTT 回调调用同步 API。不需要额外的 RTOS 示例包装源文件。

本轮完成源码/编译检查，最新硬件回归待 F407 接回。真正通过需同时记录
下载 CRC、pending、切槽 VTOR、active、健康确认及 Fault 寄存器，不能只看返回值。

板级初始化日志时，先设置 `stm_log_set_tick(HAL_GetTick)`，再使用
`stm_log_init_output(output, STM_LOG_LVL_INFO)`。UART 和 RTT 都由应用回调提供，
不能把诊断日志输出到 ESP-AT 协议使用的 USART2。完整后端例子见 stm_log README。
