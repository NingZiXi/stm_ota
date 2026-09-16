# stm_ota 维护手册

本文面向维护 `stm_ota` 的开发人员。普通使用者先阅读 [README.md](README.md) 和
[example/README.md](example/README.md)。

## 1. 库的职责与边界

`stm_ota` 负责：

- 通过 `esp_at_client` 原始 TCP 发送 HTTP Range 请求；
- 在擦除 Flash 前探测远端版本、槽位、大小和整包 CRC；
- 分片下载到 STM32F407 片内 Flash；
- 每片写入后回读 CRC，结束时校验接收 CRC、Flash CRC 和服务端 CRC；
- 在备份寄存器仍保留时恢复已完成的下载前缀；
- 当前 Range 失败时关闭连接并最多重试 3 次。

`stm_ota` 不负责：

- Wi-Fi、MQTT 连接和更新指令来源；
- 当前版本与目标版本的业务策略；
- A/B active、pending、boot_count 或健康确认协议；
- Bootloader metadata、签名或公钥验证；
- 完全掉电后保证续传；
- 创建 RTOS 任务或动态分配内存。

正确职责链为：

```text
应用收到更新请求
  → stm_ota_probe()
  → 应用检查槽位/版本/大小
  → stm_ota_download()
  → 应用写 pending 槽
  → 系统复位
  → Bootloader 校验 metadata/CRC/签名并试启动
  → 新 App 健康检查成功后提交 active
```

## 2. 目录职责

```text
stm_ota/
├── inc/stm_ota.h           公开 API
├── src/stm_ota.c           Range、Flash、CRC 和断点实现
├── example/                A/B 业务适配及裸机/FreeRTOS 示例
├── CMakeLists.txt          stm_ota 静态库目标
├── README.md               使用说明
└── ADMIN.md                本维护手册
```

库当前直接依赖 STM32F4 HAL 和 `esp_at_client`。移植到其他 STM32 系列时，必须单独适配
Flash 扇区布局、擦除粒度、编程粒度和地址范围，不能只替换头文件。

## 3. 下载数据流

### 3.1 探测

`stm_ota_probe()` 请求前 8 字节，但主要使用 HTTP 响应头：

- `206 Partial Content`；
- `Content-Range`；
- `X-CRC32`；
- `X-Firmware-Version-Code`；
- `X-Firmware-Slot`；
- `X-Firmware-Size`。

探测不会擦除或写入 Flash。应用必须在探测后检查目标槽、版本门禁和容量，再开始下载。

### 3.2 下载

每个分片执行：

1. 发送精确 Range；
2. 校验 HTTP 状态和 Content-Range；
3. 校验响应头中的整包 CRC 没有变化；
4. 计算接收分片 CRC；
5. 按 word 写入 Flash；
6. 从 Flash 回读并校验分片 CRC；
7. 保存下一偏移与前缀 CRC；
8. 调用进度回调。

结束时再次计算整个目标包的 Flash CRC，只有
`接收 CRC == Flash CRC == 服务端期望 CRC` 才返回 `STM_OTA_OK`。

## 4. Flash 安全约束

当前实现仅允许 A/B 槽的合法包范围，边界来自主工程共享的
`common/boot_state_protocol.h`。维护时必须保证：

- `download_addr` 是合法槽位基址，不能是当前正在执行的槽；
- `total_size > 0` 且结束地址不溢出；
- 包不能越过目标槽边界；
- `chunk_size` 在 4～1400 字节之间且 4 字节对齐；
- 所有参数在 `HAL_FLASH_Unlock()` 和擦除之前校验；
- `HAL_FLASH_Unlock()`、擦除和每次编程的返回值都必须检查；
- 任一失败路径关闭 TCP、重新锁定 Flash，并按语义清理断点。

Flash 擦写期间不要执行位于被擦除槽中的代码，也不要让中断向量指向目标槽。A/B 应始终下载到
当前运行槽的另一槽。

## 5. 断点状态

`BKP3R..BKP10R` 用于运行期下载断点，`BKP1R/BKP2R` 留给 Bootloader 状态协议：

| 寄存器 | 内容 |
| --- | --- |
| `BKP3R` | 断点 magic，最后写入 |
| `BKP4R` | 目标下载地址 |
| `BKP5R` | 整包大小 |
| `BKP6R` | 整包 CRC32 |
| `BKP7R` | 分片大小 |
| `BKP8R` | 下一下载偏移 |
| `BKP9R` | 已完成前缀 CRC32 |
| `BKP10R` | 状态结构 CRC32 |

恢复条件全部满足才接受断点：参数一致、状态 CRC 正确、偏移合法且 Flash 前缀 CRC 一致。
magic 最后写入，避免复位发生在状态更新中间时误接受半写状态。

重要限制：没有 VBAT 或备份域未保持时，完全断电会丢失断点。本库只把断点作为软复位、看门狗
复位或备份域仍保留时的优化；完全掉电后自动整槽重下是预期行为，不承诺掉电续传。

## 6. 失败语义

| 错误码 | 含义 | 应用建议 |
| --- | --- | --- |
| `STM_OTA_ERR_INVALID` | URL、范围、大小或分片配置非法 | 修正配置，不重试 |
| `STM_OTA_ERR_HTTP` | TCP/Range/HTTP 响应失败 | 网络恢复后重新触发完整流程 |
| `STM_OTA_ERR_FLASH` | 解锁、擦除、写入或回读失败 | 保留旧 active 槽，记录硬件故障 |
| `STM_OTA_ERR_CRC` | 响应头 CRC 改变或整包 CRC 不一致 | 拒绝 pending，重新发布或下载 |
| `STM_OTA_ERR_TIMEOUT` | 预留的超时分类 | 按网络故障处理 |

任何失败都不能写 pending 或修改 active。只有 `stm_ota_download()` 返回 `STM_OTA_OK`，并且应用
再次核对 metadata/版本后，才允许请求 Bootloader 试启动目标槽。

## 7. 与 Bootloader 的集成边界

库不知道项目如何编码 active/pending，也不直接依赖 Bootloader 头文件。应用层必须提供：

- 根据 `SCB->VTOR` 识别当前运行槽；
- 选择另一槽和对应 URL/地址；
- 读取当前/目标镜像版本；
- 写入一次性 pending 请求并读回确认；
- 复位；
- 新 App 健康检查成功后提交 active。

`stm_ota_request_reboot()` 是兼容旧工程的通用复位标志接口，只写旧的 BKP0R magic。
当前 A/B 工程不应使用它代替 pending 协议。

metadata v2 签名由构建工具写入，Bootloader 校验。下载库只保证传输完整性，不能把 CRC32 当作
真实性认证。

## 8. RTOS 与调用上下文

`stm_ota_probe()` 和 `stm_ota_download()` 是同步阻塞 API：

- 裸机在主循环上下文调用；
- FreeRTOS 在唯一网络/OTA 任务中调用；
- ESP-AT MQTT 回调只设置 `ota_requested`，不能直接下载；
- 下载期间不允许另一个任务并发调用 ESP-AT 同步 API；
- 进度回调应短小，不能再次调用 OTA 或 ESP-AT 同步接口。

库源码不得引入 FreeRTOS/CMSIS-RTOS 头文件、任务、队列、信号量或 heap。

## 9. 服务端契约

服务器必须对每个合法 Range 返回精确的 `206`、一致的总大小和稳定的整包 CRC。以下情况必须
视为协议失败：

- 返回 200 而不是 206；
- Content-Range 起止位置或总长度不匹配；
- body 长度与请求 Range 不一致；
- 下载过程中 X-CRC32 改变；
- 槽位、版本或包大小响应头缺失/非法。

发布系统应先完成签名和 `--require-signature` 门禁，再允许生产设备下载。

## 10. 测试与故障注入

修改 Range、Flash、CRC、断点或清理路径后至少验证：

1. 合法 A→B 和 B→A 下载；
2. 首片、中间片和末片网络中断；
3. `CONFIG_OTA_TEST_FLASH_FAIL_CHUNK` 注入 Flash 写失败；
4. 错误 CRC、错误槽位、错误包大小；
5. 软复位后断点接受；
6. 参数变化、断点 CRC 错误或 Flash 前缀变化时拒绝断点并重下；
7. 下载失败后旧 active 槽仍可启动；
8. 下载成功后 pending、跳转和健康确认闭环；
9. `CFSR=0`、`HFSR=0`，无 HardFault。

## 11. 发布清单

- [ ] README、ADMIN、公开头文件和示例保持一致；
- [ ] Debug、Debug-B、Baremetal、Baremetal-B 构建通过；
- [ ] Release-OTA A/B 签名包门禁通过；
- [ ] 最新签名包完成一次真实 A→B 冒烟；
- [ ] 失败路径未污染 pending/active；
- [ ] 库源码静态检查无 RTOS 和动态内存依赖；
- [ ] `git diff --check` 通过；
- [ ] 子库独立提交后再更新上层工程引用。
