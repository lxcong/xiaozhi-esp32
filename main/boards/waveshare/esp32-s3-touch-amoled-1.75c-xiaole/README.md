# 小乐：Waveshare ESP32-S3-Touch-AMOLED-1.75C

仅用于 **1.75C / ESP32-S3 / 32 MB flash / 466×466 CO5300**，不能用于非 C 版。
从上游 `waveshare/esp32-s3-touch-amoled-1.75` 的 C 版接线派生；原板型保持不变。
独立 type/name：`esp32-s3-touch-amoled-1.75c-xiaole`。定制更新需要使用此型号的固件；
不假定小智公共 OTA 服务已经发布或支持这一新型号。不要用原板型自动更新覆盖它。

## 行为

- 原有 21 种小乐表情，由表情引擎（`xiaole_frame.h`，结构参照 groki-bot 的 avatar）决定每帧画哪张：
  叠加表情（带保持时长，如唤醒时 600 ms 眨眼）> 语音状态（thinking 显示 thinking 脸）> 服务器
  `llm.emotion` 设定的基础表情。表情切换在 300 ms 内按像素交叉淡入，切换中被打断时把当前混合
  冻结为新的起点而不是跳变；摇晃惊讶和睡眠是反射，不做过渡。
- 待机随机眨眼（睁 2.5–4.5 秒，闭 180–260 ms）、视线小幅游移（0.6–2.4 秒换一次，±1 像素），
  轻微上下呼吸。听的时候身体抬高 2 像素，想的时候视线斜上。
- 口型：板级 `XiaoleAudioCodec` 在播放路径上对每个 60 ms PCM 块算 RMS（dBFS）交给显示；
  `xiaole::LoudnessEnvelope`（照 groki 的 update_mouth：相对剪辑滑动平均的响度，快起慢落）
  得到 0..1 的响度，说话时驱动身体纵向压缩（最多 12%）和嘴型叠加：≥0.2 半张、≥0.55 张开。
  嘴型图层由 `assets/xiaole/generate.py` 的 `mouth_overlay()` 生成，`export_firmware.py` 导出为
  `kMouthOverlays`（10×8，覆盖 x 28..37、y 40..47）。播放块超过 250 ms 没更新即视为静音闭嘴。
- QMI8658 加速度与角速度驱动平滑跟随。水平位移 ±24 px、垂直 ±12 px，耳朵延迟摆动。
- 开机 / 唤醒后静止约 0.6 秒校准中心；校准前保持待机动画。
- 两次明显加速度脉冲触发 850 ms 惊讶表情，冷却 2.5 秒，然后恢复当前对话表情。
- 字幕固定在 x=83…383、y=326…398，不随兔子移动。文字按 36 px 行距换行，两行以内居中静止；
  更长的内容按行步进滚动：停 1.5 秒、0.3 秒滑一行，到底后停 1.5 秒回到开头。
  几何和步进曲线在 `xiaole_frame.h`，由 `tests/xiaole/frame_test.cc` 固定；LVGL 侧在 `xiaole_lvgl.h`。
- 传感器缺失或数据过期时回到待机动画，并重试。睡眠时关闭 IMU 两个传感器，动画降至 1 fps。
- 不在动作回调里唤醒麦克风、发消息或改变对话状态。
- 触摸读取由板级自己的 LVGL 输入回调完成，只在 CST9217 拉 INT 后才读（轮询时芯片每帧都回
  非 ACK）。读失败只记一次警告、保持上一状态，不像 esp_lvgl_port 那样 ESP_ERROR_CHECK abort。
  触摸、IMU、PMIC 的 I2C 访问共用一把互斥锁，避免插进 CST9217 两段式事务的间隙。
- AXP2101 用板级的不 abort 版本访问：这条总线上挂着 PMIC、触摸、IMU、ES8311、ES7210，偶发
  NACK 时上游 `I2cDevice::ReadReg` 的 ESP_ERROR_CHECK 会让整机重启；现在读失败保留上次电量，
  连续失败 10 次复位总线。

传感器在低优先级独立任务中采样，I2C 每笔超时 10 ms；显示只读取固定大小快照。
图像在 LVGL 锁内更新，20 fps；约 84 KiB 只读调色板素材 + 18 KiB 可变像素缓冲。
设备服务与 Board 同寿命，不支持在运行中销毁 Board。

## 构建

ESP-IDF 最低 6.0.1（本次使用 6.0.1）。在仓库根目录运行：

```sh
python scripts/build.py waveshare/esp32-s3-touch-amoled-1.75c-xiaole \
  --name esp32-s3-touch-amoled-1.75c-xiaole --language zh-CN
```

项目本地工具链位于工作区 `tooling/esp-idf`，工具缓存位于 `tooling/idf-tools`。
使用该环境前设置 `IDF_PATH`、`IDF_TOOLS_PATH`，用 `tooling/idf-tools/python_env/idf6.0_py3.12_env/bin/python`
执行 `$IDF_PATH/tools/idf_tools.py export` 并 eval 其输出（`export.sh` 需要系统 python3 ≥ 3.10）。

板型配置使用 `partitions/v2/32m_waveshare_amoled_1.75c.csv`，与设备出厂分区表逐字节一致，
并打开 `CONFIG_IDF_EXPERIMENTAL_FEATURES` + `CONFIG_BOOTLOADER_CACHE_32BIT_ADDR_QUAD_FLASH`：
设备的 assets 分区在 0x11f0000，超过 16 MB，没有这个选项 app 无法 mmap 资源，唤醒词模型不会加载。
微雪自带的 bootloader（IDF 5.5.2）已支持 32 位地址，无需替换。

**构建默认资源使用“你好小智”；交付前必须运行工作区
`assets/xiaole/package_firmware.py`。** 此脚本使用上游资源打包器生成
`artifacts/xiaole-motion/xiaole-assets.bin`，包含 MultiNet6 中文模型 mn6_cn **和 fst 模型**
（commands_cn/en.txt；MultiNet6 的 model_init 会引用它，缺了会空指针崩溃），命令
`xiao le` / “小乐”，阈值 0.2，识别时长 3000 ms。资源上限按设备 assets 分区取 9 MB。
固件音频引擎会从资源中的模型选择 MultiNet。脚本不会执行刷写。

小乐图像由工作区 `assets/xiaole/generate.py` 生成，使用
`assets/xiaole/export_firmware.py` 重建本目录 `xiaole_sprites.h`。
修改美术素材后先导出，再编译。`preview_motion.py` 的 GIF 是动效示意，不是真机截图。

## 验证

仓库根目录：

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I main/boards/waveshare/esp32-s3-touch-amoled-1.75c-xiaole \
  tests/xiaole/motion_test.cc -o /tmp/xiaole-motion-test
/tmp/xiaole-motion-test
python -m unittest discover -s scripts/tests -v
```

主机测试覆盖校准、倾斜、摇晃冷却、无效输入、运动边界、字幕四角圆屏几何、换行行数和步进滚动曲线；`tests/xiaole/frame_test.cc`
以同样方式编译 `xiaole_frame.h`，覆盖表情选择、眨眼窗口、耳朵行偏移与位置偏移。

像素合成（`xiaole_frame.h`）与 LVGL 布局/精灵描述（`xiaole_lvgl.h`）从 `xiaole_display.cc`
抽出，供固件与工作区 `tools/xiaole-sim`（macOS 上 LVGL 9.5 + SDL2 的主机模拟器）共用：
模拟器用鼠标/键盘驱动假 IMU，按 `xiaole_imu.cc` 的 20 ms 采样与 500 ms 过期规则喂给
`xiaole::Motion`，渲染的待机画面与真机同一代码路径。调动画先在模拟器里看，再编译刷机。

真机（2026-10-19，`device-backup/7c0c5f42416c/sixth-boot-ota1.log`）：从 ota_1 启动，assets 映射
6272 KB，MultiNet6 加载“xiao le”，Wi-Fi/MQTT 连接，“小乐”被识别（prob 0.37）并完成一轮
对话与播音，4 分钟内无重启。仍需人工检查轴方向、眨眼/耳朵效果、触摸、两行中文/超长文本滚动、
休眠恢复、断网重连，以及边摇晃边说话时的录音、播音、打断、AEC 和唤醒成功率。

## 真机写入方式（2026-10-19 已执行）

设备 MAC 7c:0c:5f:42:41:6c，整片备份在工作区 `device-backup/7c0c5f42416c/`。出厂布局：
factory=brookesia 启动器（0x110000, 9M），ota_0=微雪小智 2.1.0（0xa10000），ota_1 空（0xe00000），
assets 9M（0x11f0000），nvs 与本仓库 v2 布局同址。保留 bootloader、分区表、nvs、factory、ota_0：

```sh
python -m esptool --chip esp32s3 -p <串口> -b 921600 write-flash --flash-size keep \
  0x11f0000 artifacts/xiaole-motion/xiaole-assets.bin   # 仅资源变化时
python -m esptool --chip esp32s3 -p <串口> -b 921600 write-flash --flash-size keep \
  0xe00000 artifacts/xiaole-motion/xiaole-app.bin
IDF_PATH=tooling/esp-idf python tooling/esp-idf/components/app_update/otatool.py \
  --port <串口> switch_ota_partition --slot 1               # 首次切换
```

首次切换实际改用直接写 otadata（seq=2、state=VALID、crc，8 KB 镜像写到 0x10d000）：
otatool 写出的条目 state 为 UNDEFINED，期间曾出现过一次启动槽被改回 ota_0、再被 2.1.0 擦成
factory 的情况，原因没能从设备上取证（IDF 6.0 bootloader 源码只把 NEW 转 PENDING_VERIFY），
写成 VALID 后任何回滚逻辑都不会再动它。`otatool.py erase_otadata` 可回到出厂启动器。
assets 分区与 2.1.0 共用：2.1.0 读到小乐资源会崩溃（它没有 fst 容错），回退前先写回备份里的资源。不要执行 `idf.py flash`：它会覆盖 bootloader、
分区表（虽然相同）并把 app 写进 factory 分区，覆盖微雪启动器。

省电定时器空闲 60 秒进入省电显示，不做空闲自动关机：AXP2101 的电流方向位在 USB 供电下也会
报告“放电”，按它判断仍会关机（真机验证过），所以关机只靠电源键长按 4 秒。上游 1.75 板空闲 5 分钟关机。

## 服务器：本机 groki 网关

`config.json` 把默认 OTA 地址（`CONFIG_OTA_URL`）设成本机网关 `http://192.168.10.58:8766/xiaozhi/ota/`
（工作区 `integrations/groki-gateway`，分支 xiaole-ota-endpoint）。网关在 OTA 响应里给出
`ws://192.168.10.58:8765/` 和令牌，固件因此改走网关的小智协议服务，由网关接 Gemini Live 或
Grok Bot 登录态语音。NVS 里的 `wifi/ota_url`（配网页面“自定义 OTA 地址”）非空时优先于这个默认值，
填回 `https://api.tenclass.net/xiaozhi/ota/` 即可回到小智官方服务器。Mac 的局域网 IP 变了要同步改。

## 硬件依据

- [微雪官方示例与轴向映射](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C/tree/main/examples/esp-idf/04_Immersive_block)
- [微雪 QMI8658 寄存器定义与数据缩放](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/sensor/qmi8658)

使用 0x6B（兼容探测 0x6A）、WHO_AM_I=0x05、加速度 ±8g / 125 Hz、
角速度 ±512°/s / 125 Hz。传感器 Y 映射画面 X，传感器 X 映射画面 Y；实际方向待真机确认。
