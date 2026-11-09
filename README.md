# dsh-pet-esp32

把 [DeepSeek Harness](https://github.com/deepseek-ai) 的桌宠 **[dsh-pet](https://github.com/PC2005-cloud/dsh-pet)**
（蓝毛小女仆）搬到 **M5Stack Dial v1.1** 的圆屏上：她自己待机、随机整活，
DSH 在思考/干活/出错时会跟着变状态，转旋钮还能一章一章翻动作。

角色美术、动画、状态台词全部来自原作者 **PC2005-cloud** 的 dsh-pet（MIT），
本项目只做「重新编码 + 上设备」。详见文末[致谢](#致谢)。

| 待机 | 跟着 DSH 干活 |
| --- | --- |
| ![idle](docs/dial-idle.png) | ![working](docs/dial-working.png) |

## 能做什么

- **26 段动画、559 帧、4.8 MB**，全部塞进 Flash，开机即用，不联网。
- **自动模式**：待机呼吸循环，每隔 8–16 秒随机来一段小动作（吃白饭、玩魔方、跳舞……）。
- **转旋钮**：慢慢转一格切一个；快速转就连续切；反转立刻往回；停手就停。
- **按键 / 触摸**：短按随机「点击回应」，长按睡觉 / 唤醒。
- **DSH 联动**：桌面端在跑任务时，屏幕上会显示 dsh-pet 自己的中文台词
  （「正在处理任务呢，别打扰我~」这类），并切到对应的工作动画。
- **不闪**：画面、字幕在同一次推屏里合成，10fps 下没有闪烁。

## 硬件

- M5Stack Dial v1.1：ESP32-S3FN8（8 MB Flash，**无 PSRAM**）、1.28" 240×240 圆屏、
  旋钮编码器 + 按键 + 电容触摸。
- 固件不开 WiFi/蓝牙；USB 供电或内置电池都能跑。

> Waveshare ESP32-S3-Touch-AMOLED-1.75C（466×466）的移植在 `firmware_175c/`，
> **尚未点亮**，面板 QSPI 初始化还在调，先别当成品用。

## 怎么用

| 操作 | 效果 |
| --- | --- |
| 慢慢转一格 | 切到下一个动画（20 秒不动回到自动轮播） |
| 快速转 | 连续切换，松开即停 |
| 反向转 | 立刻回退 |
| 短按 / 点屏 | 随机播一个「点击回应」 |
| 长按 | 睡觉 / 唤醒 |

屏幕底部一行大字就是当前动画名；接入 DSH 后换成那句中文台词。

## 和 DSH 联动

dsh-pet 已经自带状态接口，桥接脚本只读它，**不改 DSH 本体**：

```powershell
node bridge\dsh-pet-dial-bridge.mjs --port COM5
```

它会：

1. 自动找到设备（探测 `VID_303A&PID_1001` 串口，发 `I` 看是不是本固件）；
2. 长轮询 `sections.workStatus.data.state`，把 `thinking / working / result /
   waiting / success / error / idle` 转成设备命令；
3. DSH 安静 90 秒后，设备自动回到独立轮播。

参数：`--port COM5`、`--url http://127.0.0.1:52202`（默认读 `$env:DSH_WEB_URL`）、
`--pet main`。也可以直接双击 `bridge\start-bridge.cmd`。

## 动画是怎么来的

1. 从 dsh-pet 的 `assets/webm` 里按 10fps 抽帧（ffmpeg，保留 alpha）；
2. 统一裁成方形、缩放、合成到深色背景；
3. 感知去重、按原始时长记每帧停留；
4. JPEG q80 打包成自定义二进制（帧表 + 文本位图 + 数据）。
5. 状态台词用 dsh-pet 自带的「上首软糖体」在构建时渲染成 1bpp 位图，
   设备端不需要中文字库。

重新生成（需要 dsh-pet 已安装、ffmpeg、Pillow）：

```powershell
python tools\build_assets.py                     # 240x240（Dial）
$env:PET_SIZE="466"; $env:PET_CROP="140,4,360,360"; $env:PET_OUT="firmware_175c\data"
python tools\build_assets.py                     # 466x466（1.75C）
```

## 构建与烧录

```powershell
pwsh -File tools\build.ps1            # 编译 + 烧 app
pwsh -File tools\build.ps1 -Assets    # 连素材一起烧
```

- 工具链：arduino-cli + `esp32:esp32@3.3.12` + M5Dial / M5Unified / M5GFX / JPEGDEC，
  默认装在 `C:\dshdial`（可用 `$env:DSH_DIAL_TOOLCHAIN` 覆盖）。
  路径要短：GCC 的 C++ 头文件路径超过 MAX_PATH 就打不开。
- 分区表：app 2 MB + `assets` 5.94 MB 裸数据分区（不是文件系统，直接 mmap）。
- **烧录必须 `--flash_mode dio`**：S3 bootloader 头是 DIO，用 qio 改写镜像头后
  ROM 会加载失败（`ets_loader.c 78` + 看门狗复位）。

## 串口协议

115200，按行：

| 命令 | 说明 |
| --- | --- |
| `S <state>` | `idle/thinking/working/result/waiting/success/error` |
| `A <序号\|标签>` | 立刻播放某个动画 |
| `I` | `INFO anims=… frames=… texts=… heap=… cur=… dec=…ms` |
| `P` | `PONG` |
| `E <n>` | 测试钩子：假装旋钮转了 n 个 count |
| `V` | 把画面按 2×2 采样以十六进制吐出来（`tools\capture.py` 用它截图） |

## 踩过的坑

1. **字节序**：M5GFX 16bpp 精灵缓冲是**大端** RGB565，JPEGDEC 默认小端，
   直接 memcpy 上屏就是花屏色；要 `RGB565_BIG_ENDIAN`。
2. **旋钮**：`M5Dial.Encoder.read()` 返回**绝对位置且不清零**，必须用 `readAndReset()`
   拿增量，否则「转一下就不停切」。
3. **帧时钟**：切动画时要把排帧基准重置，否则新动画会快进追帧。
4. **字幕对比度**：10px 中文加 1px 描边会糊成一坨，改成把字幕区压暗再叠白字
   （压暗要先转回本机序，绿色通道跨字节）。
5. **构图**：去掉顶部文字后角色显低，裁切窗口要上移让她垂直居中。

## 致谢

- **PC2005-cloud** —— [dsh-pet](https://github.com/PC2005-cloud/dsh-pet) 原作者：
  角色、动画、台词、字体都来自这个项目，没有它就什么都没有。
- **M5Stack** —— M5Dial / M5Unified / M5GFX，Dial 的屏幕、旋钮、触摸都是它们封装的。
- **LovyanGFX** —— M5GFX 的上游图形库。
- **Larry Bank (bitbank2)** —— JPEGDEC，240×240 每帧 13ms 解码靠它。
- **Anyvia Dial 交互 demo** —— README 里带机身外框的截图，外框配色和比例参考自它。

## 许可证

MIT，见 [LICENSE](LICENSE)。原始版权声明（PC2005-cloud）保留。
dsh-pet 的字体文件没有重新分发，只在构建时用来渲染文本位图。

## Waveshare ESP32-S3-Touch-AMOLED-1.75C (466x466)

Second target in `firmware_175c/`: CO5300 over QSPI (CS12 / CLK38 / D0-3 = 4-7 / RST1),
CST9217 touch on I2C (SDA15 / SCL14 / RST2). No encoder and no buttons, so gestures
replace the knob:

| Gesture | Effect |
| --- | --- |
| tap | next animation |
| double tap | previous animation |
| swipe left / right | next / previous |
| long press | sleep / wake |

Build/flash:

```powershell
pwsh -File tools\build_175c.ps1
```

Notes: the 466x466 blob is 11.5 MB, so frames are read from the assets partition per
frame instead of mmap (the S3 flash mapping window here tops out at 8 MB), and the
434 KB sprite is allocated explicitly from PSRAM.
