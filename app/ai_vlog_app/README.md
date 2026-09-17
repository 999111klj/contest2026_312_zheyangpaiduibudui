# AI Vlog App

基于 ESP32-S3-EYE 硬件的 AI Vlog 播放端 Android 应用，参加 OpenVela 智能硬件大赛。

## 项目简介

这是一个 Flutter Android 应用，配合 ESP32-S3-EYE 硬件和后端大模型服务，实现**自动接收、管理和播放 AI 生成的 Vlog 视频**。

**核心功能**：
- 通过局域网 HTTP 连接后端服务器
- 自动/手动接收视频到本地
- 视频播放与相册保存
- 灵活的下载位置配置

**架构流程**：
```
ESP32-S3-EYE 拍摄 → 上传后端 → 大模型生成视频 → App 通过 HTTP 拉取 → 本地播放
```

## 快速开始

### 环境要求

| 环境 | 版本要求 | 说明 |
|------|---------|------|
| Flutter SDK | 3.35.7 | **不要使用更高版本** |
| JDK | 17 | **不兼容 Java 21+** |
| Android SDK | API 36 + platform-tools | 通常随 Android Studio 安装 |

### 一键构建

1. 确保上述环境已安装且在 PATH 中（或配置脚本顶部的路径变量）
2. **工程路径必须为纯英文**（Gradle 不支持中文路径）
3. 双击运行 `build_apk.bat`（Windows）
4. 构建完成后在根目录得到 `AI-Vlog.apk`（约 48MB）

**首次构建约 10 分钟**（需下载 Gradle 和依赖），后续增量构建 1～3 分钟。

### 手动构建

```bash
cd ai_vlog_app

# 设置 JDK 17（若系统默认不是）
set JAVA_HOME=C:\Program Files\Microsoft\jdk-17.0.20.101-hotspot

# 构建 Release APK
flutter build apk --release

# 产物位置
# build\app\outputs\flutter-apk\app-release.apk
```

## 功能模块

### 1. Vlog 库（首页）
- 列出所有本地视频
- 点击播放
- 删除/导入视频

### 2. 设备页
- 输入后端 IP 地址连接
- **自动接收模式**：每 10 秒轮询，发现新视频自动下载
- **手动下载**：单独下载指定视频，已下载的也可重新拉取
- 实时显示下载进度

### 3. 视频播放器
- 全屏循环播放
- 点击暂停/继续
- 保存到手机相册

### 4. 设置页
- **下载存放位置**（三选一）：
  - 应用内部目录（默认，最稳定）
  - 应用外部目录（文件管理器可见）
  - 自定义文件夹（系统选择器，自动测试写入权限）
- 关于信息

## 后端接口约定

App 需要后端提供以下 HTTP 接口：

### GET /list
返回视频列表：
```json
[
  {
    "name": "video.mp4",
    "size": 123456,
    "title": "视频标题",
    "url": "https://192.168.1.100/download?name=video.mp4"
  }
]
```

### GET /download?name=xxx.mp4
返回视频文件流，需要 Bearer Token 认证：
```http
GET /download?name=video.mp4
Authorization: Bearer YOUR_API_TOKEN
```

## 安全配置

**重要**：上传到 GitHub 前，请先替换 `ai_vlog_app/lib/services/device_client.dart` 中的硬编码 Token：

```dart
// 第 48～50 行
// TODO: 替换为后端部署时生成的 API Token
// 后端启动时会生成一个随机密钥，将其填入下方 _apiToken 常量中
static const _apiToken = 'YOUR_BACKEND_API_TOKEN_HERE';
```

**部署时**：
1. 后端生成/配置 API Token
2. 将上述占位符替换为真实 Token
3. 重新构建 APK

## 项目结构

```
ai_vlog_app/
├── lib/
│   ├── main.dart                    # 应用入口 + 底部导航
│   ├── pages/
│   │   ├── vlog_library_page.dart   # Vlog 库
│   │   ├── device_page.dart         # 设备连接
│   │   ├── player_page.dart         # 视频播放
│   │   └── settings_page.dart       # 设置
│   └── services/
│       ├── device_client.dart       # HTTP 通信（需配置 Token）
│       ├── vlog_store.dart          # 本地视频管理
│       └── settings_store.dart      # App 设置持久化
├── android/                         # Android 原生配置
├── build_apk.bat                    # 一键构建脚本（Windows）
├── build_apk.ps1                    # 构建脚本（PowerShell）
└── pubspec.yaml                     # 依赖配置
```

## 开发调试

### Mock 服务器
项目提供 `mock_device_server.py`，用于本地测试（无需真实硬件）：

```bash
python mock_device_server.py
# 默认监听 0.0.0.0:8080
```

### 真机联调
1. 启动 Mock 服务器或真实后端
2. 手机与电脑连接同一网络
3. 安装 APK：`adb install -r AI-Vlog.apk`
4. App 内输入电脑 IP（如 `192.168.1.100:8080`）
5. 测试自动接收、手动下载、播放、保存相册

## 技术细节

### 自动接收逻辑
- 开启开关后每 10 秒调用 `/list`
- 通过文件名和标题判断是否已存在
- 新视频自动后台下载，完成后通知用户

### 判重机制
- 提取本地视频原始文件名（去掉时间戳前缀）
- 同时记录所有标题
- 远程视频文件名或标题匹配则标记"已在本地"

### 错误处理
- 连接超时/失败：友好提示检查网络和 IP
- 下载失败：自动接收模式静默，手动下载弹错误
- 轮询失败：静默（不打断用户）

## 已知问题

1. **Kotlin 版本警告**：当前 1.9.24，Flutter 未来版本将要求 2.1.0+（不影响当前使用）
2. **HTTPS 证书跳过**：开发环境跳过内网自签名证书校验，生产环境需评估安全风险
3. **播放依赖绝对路径**：下载后手动移动文件或切换存储位置会导致播放失败

## 后续优化方向

- [ ] 视频缩略图预览
- [ ] 下载队列管理
- [ ] 断点续传
- [ ] 网络断线重连
- [ ] Kotlin 升级到 2.1.0+

## 许可证

本项目为 OpenVela 智能硬件大赛参赛作品。

---

**联系方式**：如有问题请提交 Issue
