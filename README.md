# VlogEye —— 拍照即成片的端云协同 AI 视频日志相机

## 一、作品简介

在 ESP32-S3-EYE（openvela / NuttX）上实现**拍照即成片**的端云协同 AI 视频日志相机：板端 OV2640 取流，NIMA 美学评分模型（int8 量化，309 KB，TensorFlow Lite Micro）实时打分，高分照片（≥85）经 dHash 去重后自动写入 SD 卡持久化队列；支持**完全离线拍摄**，回网后指数退避自动补传；云端服务器调用大赛 MiMo（mimo-v2.5）看图生成分镜与中文文案，FFmpeg 自动渲染运镜、转场与字幕成片。全程无需人工挑选与剪辑——**按下快门，剩下的交给流水线**。

核心亮点：

1. **端侧 AI 筛选**：NIMA int8 推理（单帧约 1.4 s，arena 1 MB）做第一道筛选，只上传高分素材，端云带宽与云端成本大幅下降；
2. **弱网鲁棒**：SD manifest 状态机持久化队列，断电续传、跨重启重试（实测旧会话连续多日开机自动续传）；懒连接 + 指数退避（5 s→300 s）；
3. **openvela SDMMC 驱动缺陷修复**：ESP32-S3 SDMMC IDMAC 只能寻址内部 SRAM，原驱动把 PSRAM 指针直接填入 DMA 描述符导致写卡数据损坏；修复为静态对齐内部弹射缓冲（见 `patches/` 与 nuttx PR），可回馈上游；
4. **TLS 证书锁定**（SHA-256 pin）+ 无 RTC 板子的时间基线强制；LLM 输出 3 次重试 + 确定性本地兜底，离线/模型异常时功能不缺失；
5. 实测两次真实拍摄会话全链路无人工干预：5 张照片全部字节级完整上传（201），视频约 83 秒自动生成，字幕为真实画面描述。

## 二、选题方向

AI 硬件产品创新。理由：面向"记录意愿强但剪辑能力弱"的人群（银发族、儿童家长、户外爱好者），把 Vlog 制作的"挑选 + 剪辑"压缩到拍摄瞬间自动完成，端侧 AI（TFLM 量化推理）与云端多模态 LLM 各司其职，是典型的端云协同 AI 硬件产品形态。

## 三、目录结构

```text
app/camera_gallery/       # 端侧应用全部源码（经 manifest linkfile 软链到
                          # packages/demos/contest2026_312_camera_gallery 参与编译）
  ├── camera_gallery_main.c        # 主程序（取景/UI/按键/评分调度）
  ├── camera_gallery_aivlog.c      # AI Vlog 模式（NIMA 评分/dHash 去重/入队）
  ├── camera_gallery_aivlog_cloud.c# 云上传 worker（manifest 状态机/退避重传/TLS）约 2100 行
  ├── camera_gallery_aivlog_model.cc / _model_data.S  # TFLM 推理 + 模型嵌入(.incbin)
  ├── camera_gallery_zh_font.c     # 自建中文字库（generate_zh_font.py 生成）
  ├── camera_gallery_level.c       # 加速度计水平辅助
  ├── camera_gallery_provision*.c  # 配网（本提交未启用 BLE 配网）
  ├── nima_exp4_int8.tflite        # NIMA int8 模型（309 KB）
  └── tools/receive_photo.py       # 辅助：经串口接收照片
third_party/esp-nn/       # espressif/esp-nn（TFLM Xtensa 优化内核；openvela
                          # manifest 未包含，随本仓提供，linkfile 到 apps/mlearning/esp-nn）
cloud/aivlog_server/      # 云端服务（FastAPI + uvicorn，HTTPS 443 + Bearer 设备鉴权）
  ├── aivlog_server/               # app/api/storage/worker/mify/renderer/config
  ├── run.py / requirements.txt    # 启动入口与依赖（另需系统 ffmpeg）
  └── server.env.example           # 配置模板（token/密钥/证书路径，复制为 server.env 并填值）
configs/aivlog/defconfig  # 实际使用的完整构建配置（拷为 nuttx/.config）
patches/                  # 各仓改动 diff：nuttx/apps（PR fallback）、ai_agent、
                          # vendor/espressif、esp-hal spinlock（见 4.2）
logs/                     # AI Coding 日志（按成员 GitHub 账号分目录）
README.md                 # 本文件
```

## 四、运行方式

### 4.1 拉取工程（评委视角）

```bash
repo init -u https://github.com/open-vela/contest2026_312_zheyangpaiduibudui \
  -b dev-ai-contest-2026 -m contest2026_312_zheyangpaiduibudui.xml
repo sync -c -j8
```

manifest 会把 `app/camera_gallery` 软链到 `packages/demos/contest2026_312_camera_gallery`、
`third_party/esp-nn` 软链到 `apps/mlearning/esp-nn`，构建系统自动发现（Kconfig/Make.defs 自注册）。

### 4.2 应用补丁（重要）

manifest 已将 nuttx / apps / ai_agent / vendor_espressif 四仓**钉定到本作品验证过的修订**
（见 `contest2026_312_zheyangpaiduibudui.xml` 中的 `revision=`），下述补丁在这些基线上必定干净应用。

**第一步（必须）**：应用 ai_agent 与 vendor/espressif 补丁——端云链路（vela_tls 证书锁定
传输、config_store 持久化、network_manager 弱网管理）与板级支持（LCD 初始化、按键驱动、
应用自启动）依赖这些修改：

```bash
cd <工作区>
git -C packages/ai_agent apply contest2026_312_zheyangpaiduibudui/patches/ai_agent-fixes.patch
git -C vendor/espressif apply contest2026_312_zheyangpaiduibudui/patches/vendor-espressif-esp32s3-eye.patch
```

**第二步（必须）**：应用 nuttx / apps fallback 补丁。本作品的驱动级修复已按比赛规则向公共仓
`dev-ai-contest-2026` 分支提交 PR（nuttx PR：【待填链接】，SDMMC IDMAC 弹射缓冲、USB-CDC
控制台防死锁、WiFi 静态 TX 缓冲、esp-hal spinlock 补丁等 16 文件；apps PR：【待填链接】，
mbedtls `-isystem` 构建修复、wapi 密钥材料清理，2 文件）。由于 manifest 钉定的是提 PR 前的
修订，**评委构建请始终应用**下列补丁（与 PR 是否合入无关）：

```bash
git -C nuttx apply contest2026_312_zheyangpaiduibudui/patches/nuttx-esp32s3-fixes.patch
cp contest2026_312_zheyangpaiduibudui/patches/esp-hal-3rdparty-spinlock.patch \
   nuttx/arch/xtensa/src/esp32s3/          # 构建时自动 apply 到 esp-hal-3rdparty
git -C apps apply contest2026_312_zheyangpaiduibudui/patches/apps-fixes.patch
```

### 4.3 编译烧录（板端）

```bash
cd nuttx
cp ../contest2026_312_zheyangpaiduibudui/configs/aivlog/defconfig .config
make olddefconfig
make -j4
make flash ESPTOOL_PORT=/dev/ttyACM0 ESPTOOL_BINDIR=./   # 需 esptool（pip 安装）
```

工具链：`xtensa-esp32s3-elf`（openvela 预编译工具链，见 AI 硬件赛道《ai_agent 上手指南》环境搭建章节），将其 bin 目录加入 PATH。

> **注意（kconfig 工具）**：`make olddefconfig` 需要支持 `osource` 语法的 kconfig 工具，请先
> `pip3 install kconfiglib`（NuttX 构建系统检测到 kconfiglib 后会自动优先使用）。
> `prebuilts/build-tools` 自带的 `kconfig-conf` 版本较老、不识别 `osource`，会在
> lvgl/frameworks 等公共仓 Kconfig 上误报语法错误。本仓库 `configs/aivlog/defconfig`
> 为完整 `.config`，即使跳过 `olddefconfig` 直接 `make -j4` 亦可编译。

### 4.4 云端服务部署（任意 Linux + Python 3.10+）

```bash
cd contest2026_312_zheyangpaiduibudui/cloud/aivlog_server
python3 -m venv venv && venv/bin/pip install -r requirements.txt   # 另需系统 ffmpeg
cp server.env.example server.env && chmod 600 server.env
# 编辑 server.env：AIVLOG_DEVICE_TOKEN（自定义随机串）、MIFY_API_KEY、TLS 证书路径
# HTTPS 需自签证书；证书 SHA-256 写入设备端（4.5 步）
venv/bin/python run.py    # 监听 0.0.0.0:443，后台 GenerationWorker 自动轮询
```

### 4.5 设备配置与使用

nsh 串口（/dev/ttyACM0，注意：打开串口的 DTR/RTS 会复位板子，用支持禁用控制线的终端）：

```bash
# 1. 配网（凭据持久化，重启自动连接）
set_wifi <ssid> <password>

# 2. 配置云端（token 与 server.env 一致；cert_sha256 为服务器证书指纹，http 用 - 代替）
camera_gallery set_aivlog_backend https <服务器IP> 443 <token> <cert_sha256|-> [/api/v1/aivlog]
camera_gallery aivlog_cloud_status    # 查看配置状态
```

使用：LCD 实时取景（NIMA 每 800 ms 评分，≥85 自动入队）→ **PLAY 键**拍摄存档 / **MENU 键**结束会话 → worker 自动连接 WiFi、TLS 上传 → 云端约 80-90 秒成片（`GET /api/v1/aivlog/sessions` 查询、`/result.mp4` 下载）。离线时拍摄与入队不受影响，回网自动补传。

**板载 AI 助手（自定义 Skill）**：应用启动时自动向板载 AI agent（packages/ai_agent）安装 `vlog-assistant` 技能（写入 `/mnt/sd/.ai_agent/skills/`，内容变更自动更新）。nsh 下运行 `ai_agent` 进入对话后可直接问「我的照片传完了吗」「上传队列什么状态」——agent 按技能指引调用 `camera_gallery aivlog_cloud_status`（已加入其 shell 白名单），用中文归纳会话、待传照片数与重试退避状态。

### 4.6 已验证结果（2026-09-17/18 实测）

| 会话 | 入队照片（NIMA 分） | 上传 | 视频生成 | AI 字幕示例 |
|---|---|---|---|---|
| 1 | 2 张（87 / 88） | create 201 + 2×asset 201 + finish 202 | ✅ ~83 s | 「深夜独处，陷入沉思」「耳机一戴，世界清净」 |
| 2 | 3 张（86 / 85 / 88） | 同上全部 201/202 | ✅ | 「办公桌前的显示器与容器」「操作键盘的特写」 |
| 3–6（09-18 复测） | 4 会话共 7 张 | 全部 201/202 | ✅ 4/4（最快 ~45 s） | 含一次 LLM 首试失败、重试成功的真实容错路径 |

照片 SD 写入→云端还原字节级一致（153,666 B/张 BMP）；09-18 另实测：设备主动回查拉取成片（result.mp4 GET 200）、固件重烧后后端配置/队列/已装 Skill 全部保留、上传 worker 自动续跑。

已知问题：低概率（约 10–20% 启动）SD 设备节点间歇性消失，硬复位即恢复（不影响已写入数据，详见技术报告）；首次上传需先完成 WiFi 关联（约 5–10 s）。

## 五、AI Coding 使用说明

- **主力工具**：Claude Code（CLI + VS Code 扩展），端侧应用（约 8000 行 C/C++）与云端服务（约 1500 行 Python）主要由 Claude Code 生成，人工负责架构决策、代码审查与实机验证；AI 代码占比约 80%。完整对话日志见 `logs/`（按成员 GitHub 账号分目录）。
- **NIMA 模型**：由团队成员使用 Kiro 完成量化与知识蒸馏训练（logs/999111klj/）。
- **AI 的关键贡献**：① SDMMC IDMAC DMA 缺陷的寄存器级定位（DMA 地址域限制 + 描述符对齐）；② 识别"USB-CDC 串口 open 即复位板子"的调试陷阱；③ BLE/WiFi 共存冲突定位（BLE 广播饿死 WiFi TX）；④ LLM 坏 JSON 问题的复现诊断与重试方案。驱动级疑难问题定位周期从按天缩短到按小时。
- **人工把关**：AI 生成的驱动修改逐行核对寄存器语义；硬件时序类问题全部经实机验证闭环。

## 六、附：openvela 能力落地

- **图形**：NX 图形栈 + ST7789 LCD 取景与中文 UI（自建中文字库）；
- **AI**：TFLite Micro 端侧推理（NIMA int8）+ ai_agent 框架 LLM 通道（云端 MiMo mimo-v2.5）+ 自定义 `vlog-assistant` Skill（agent 经 shell 白名单调用设备状态命令，自然语言查询拍摄/上传/成片进度）；
- **多媒体**：OV2640 捕获（LEDC XCLK）、BMP 编码、SD FAT 存储、FFmpeg 渲染成片。
