# Linux 后端文档

flutter_inappwebview_linux 支持两个 WebKit 后端，构建时二选一：

| 后端                  | CMake 开关值                             | 包                      | 状态               |
| --------------------- | ---------------------------------------- | ----------------------- | ------------------ |
| **WebKitGTK**（默认） | `FLUTTER_INAPPWEBVIEW_LINUX_BACKEND=gtk` | `libwebkit2gtk-4.1-dev` | 推荐，Tauri 2 同款 |
| WPE WebKit            | `FLUTTER_INAPPWEBVIEW_LINUX_BACKEND=wpe` | 见下文 WPE 章节         | 旧版，需自编译     |

## WebKitGTK 后端（默认，推荐）

### 为什么默认用它

- **发行版官方包**：`apt install libwebkit2gtk-4.1-dev` 即可（Ubuntu 22.04+ / Debian 12+ / Fedora / Arch），WPE 无官方包需 30+ 依赖自编译
- 与 Tauri 2 相同的 WebKit 移植，兼容性长期有保障

### 架构说明

| 能力                     | 实现方式                                                                                                                                                                                                                                                                                          |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| InAppWebView widget 渲染 | **GPU 直通**（默认，能力探测自动启用）：XComposite redirect + Damage 驱动 + `NameWindowPixmap` 别名 + `EGL_KHR_image_pixmap` 零拷贝导入引擎纹理；不满足时回退 **snapshot 管线**：`webkit_web_view_get_snapshot()`（VISIBLE）→ cairo ARGB32 → SIMD BGRA→RGBA → 共享三缓冲 → `FlPixelBufferTexture` |
| 帧驱动                   | GPU 直通：Damage 驱动（页面变才出帧，实测跟随页面更新率直至刷新率上限）；snapshot 回退：50ms 节拍器 + `snapshot_pending_` 防重入自动节流                                                                                                                                                          |
| 输入                     | Flutter 指针/滚轮/键盘事件合成 GdkEvent → `gtk_widget_event()`                                                                                                                                                                                                                                    |
| 离屏宿主                 | GPU 直通：override-redirect popup 定位屏外（需真实 X 窗口供捕获）；snapshot：`GtkOffscreenWindow`                                                                                                                                                                                                 |
| InAppBrowser             | webview widget 直接挂入浏览器窗口（原生渲染/输入/IME，无纹理中转）                                                                                                                                                                                                                                |
| Headless                 | 复用离屏宿主，不注册纹理                                                                                                                                                                                                                                                                          |

### GPU 直通 vs snapshot 实测（i915 / 60Hz / flutter.dev / 1280x204）

| 指标                | GPU 直通                      | snapshot 回退                        |
| ------------------- | ----------------------------- | ------------------------------------ |
| 滚动帧率            | **60fps**（打满刷新率，3.2×） | 19fps（节拍器上限钉死）              |
| 滚动 CPU（flutter） | 0.4%                          | 0.2-0.4%（持平）                     |
| 静态页 idle         | 零 present（零功耗）          | 20fps 节拍器永动机（读回+转换+上传） |
| 动画页 idle         | 跟随页面变化率（如 52fps）    | 仍 19fps（欠采样，肉眼可见卡顿）     |

GPU 直通启用条件（`WebKitGpuCapture::IsSupported`）：X11 + XComposite/XDamage 扩展 + `EGL_KHR_image_pixmap`。Wayland、Xvfb（无 DRI3，实测 `LIBGL_ALWAYS_SOFTWARE=1` 亦无效）自动回退 snapshot。可用 `FLUTTER_INAPPWEBVIEW_LINUX_GPU_CAPTURE=0` 强制回退。

### 已知限制（显式声明）

- `SendTouchEvent` 不支持（GTK3 无法合成 `GdkEventSequence`）
- `requestPointerLock/Unlock` 返回 false（WebKitGTK 无公开 API）
- ITP（`itpEnabled`）无 API 支持（宏短路为 no-op）
- snapshot 回退路径为 GPU→CPU 读回，性能上限低于 GPU 直通（高帧率场景请确保 GPU 直通启用，条件见上表）
- `WebResourceErrorType` 契约层无 Linux native value 映射（platform_interface 缺口），onReceivedError 的 error.type 降级为 IO，完整信息在 description

### 安装与使用

```bash
sudo apt-get install -y libwebkit2gtk-4.1-dev
# 默认即是 gtk 后端，无需额外参数
flutter build linux --debug
```

如需强制 WPE 后端：

```bash
cd linux && cmake -DFLUTTER_INAPPWEBVIEW_LINUX_BACKEND=wpe ...
# 或 flutter 工具链下在 linux/CMakeLists.txt 读取的缓存变量中设置
```

### 修订记录

- 2026-08：GPU 直通管线成为默认（XComposite+Damage+EGLImage 零拷贝，实测帧率 3.2×）；present 稳态零 X 往返（尺寸缓存 scale-aware）；fps debug 打点；snapshot 降级为回退路径
- 2026-08：新增 WebKitGTK 后端（`HAVE_WEBKIT_GTK`，默认）；WPE 降级为可选编译开关。渲染/输入适配实现在 `in_app_webview_gtk.cc`，API 差异收敛在 `webkit_include.h`（NetworkSession→WebContext、WebKitRectangle→GdkRectangle、WebKitColor→GdkRGBA、get_snapshot 老签名等）。

---

# WPE WebKit Backend（legacy）

The following describes how to install and configure WPE WebKit for the flutter_inappwebview Linux plugin.

## Overview

This plugin uses WPE WebKit for offscreen web rendering. WPE WebKit is the official WebKit port for embedded systems:

1. **Designed for headless/offscreen rendering** - No GTK widget hierarchy required
2. **Excellent GPU integration** - Uses DMA-BUF for zero-copy texture sharing with Flutter
3. **Lower memory footprint** - No widget hierarchy overhead
4. **Perfect for embedded systems** - Raspberry Pi, set-top boxes, kiosks, etc.

The WPE backend exports frames directly as GPU textures via DMA-BUF or SHM buffers.

## Backend Selection

The plugin supports two backend APIs, with automatic selection at compile time:

| Backend            | Package                                          | Status                 | Description                                                         |
| ------------------ | ------------------------------------------------ | ---------------------- | ------------------------------------------------------------------- |
| **WPEPlatform**    | `wpe-platform-2.0` + `wpe-platform-headless-2.0` | **DEFAULT (WPE mode)** | Modern API for WPE WebKit 2.40+. Recommended for new installations. |
| **WPEBackend-FDO** | `wpebackend-fdo-1.0`                             | Legacy Fallback        | Used only when WPEPlatform is not available. For older systems.     |

### Backend Detection Logic

The build system automatically selects the backend:

1. **If WPEPlatform is found** (`wpe-platform-2.0` and `wpe-platform-headless-2.0`):
   - WPEPlatform is used as the default backend
   - `HAVE_WPE_PLATFORM=1` is defined
   - WPEBackend-FDO is ignored even if available

2. **If WPEPlatform is NOT found** but WPEBackend-FDO is available:
   - WPEBackend-FDO is used as legacy fallback
   - `HAVE_WPE_BACKEND_LEGACY=1` is defined

3. **If neither is found**: Build fails with an error message.

> **Note:** WPEPlatform and WPEBackend-FDO are **mutually exclusive** at compile time. You cannot use both simultaneously.

## Installation Options

You can either:

1. **Use pre-built packages** from your distribution (if available): https://wpewebkit.org/about/get-wpe.html
2. **Build from source** using official tarball releases (recommended for latest features): https://wpewebkit.org/release/

### Option 2: Build from Source

#### Prerequisites

Install the **required** build dependencies (optional features are listed separately below):

```bash
# Core build tools
sudo apt-get install -y \
  build-essential cmake ninja-build meson pkg-config \
  ruby ruby-dev python3 python3-pip \
  gperf unifdef

# GLib (required)
sudo apt-get install -y libglib2.0-dev

# Networking and security (required)
sudo apt-get install -y \
  libsoup-3.0-dev \
  libssl-dev libgnutls28-dev \
  libsecret-1-dev \
  libgcrypt20-dev libtasn1-dev

# Graphics and rendering (required)
sudo apt-get install -y \
  libepoxy-dev \
  libegl1-mesa-dev libgles2-mesa-dev \
  libxkbcommon-dev

# Image and font processing (required)
sudo apt-get install -y \
  libjpeg-dev libpng-dev libwebp-dev \
  libharfbuzz-dev libharfbuzz-icu0 libfreetype6-dev libfontconfig1-dev

# Text and internationalization (required)
sudo apt-get install -y \
  libicu-dev libxml2-dev \
  libhyphen-dev libenchant-2-dev

# Media and audio (required for ENABLE_VIDEO and ENABLE_WEB_AUDIO)
sudo apt-get install -y \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-bad1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good

# Database and storage (required)
sudo apt-get install -y libsqlite3-dev

# WPE libraries (required)
sudo apt-get install -y libwpe-1.0-dev
```

> **Note:** Optional dependencies (libjxl, libavif, flite, libdrm, etc.) are listed in the "Installing All Optional Dependencies" section below. Install them for full feature support, or disable the corresponding CMake flags.

#### Optional Features and Dependencies

WPE WebKit has many optional features that are **enabled by default**. If you don't have the required dependencies installed, you must either install them or disable the feature via CMake flags.

##### Features Enabled by Default

| CMake Flag                  | Default | Min Version | Dependencies (Debian/Ubuntu)                   | Description                                    |
| --------------------------- | ------- | ----------- | ---------------------------------------------- | ---------------------------------------------- |
| `USE_JPEGXL`                | ON      | 0.7.0       | `libjxl-dev`                                   | JPEG XL image format support                   |
| `USE_AVIF`                  | ON      | 0.9.0       | `libavif-dev`                                  | AVIF image format support                      |
| `USE_WOFF2`                 | ON      | 1.0.2       | `libwoff-dev`                                  | WOFF2 web font support                         |
| `USE_LCMS`                  | ON      | —           | `liblcms2-dev`                                 | Color management (Little CMS)                  |
| `USE_ATK`                   | ON      | 2.16.0      | `libatk1.0-dev libatk-bridge2.0-dev`           | Accessibility toolkit                          |
| `USE_GBM`                   | ON      | —           | `libgbm-dev`                                   | Generic Buffer Management (GPU)                |
| `USE_LIBDRM`                | ON      | —           | `libdrm-dev`                                   | Direct Rendering Manager                       |
| `USE_LIBBACKTRACE`          | ON      | —           | `libbacktrace-dev`                             | Stack trace support                            |
| `USE_SKIA_OPENTYPE_SVG`     | ON      | —           | —                                              | Skia OpenType SVG font support                 |
| `ENABLE_SPEECH_SYNTHESIS`   | ON      | —           | See speech options below                       | Text-to-speech support                         |
| `USE_FLITE`                 | ON      | 2.2         | `flite1-dev`                                   | Flite speech engine (used when speech enabled) |
| `USE_SPIEL`                 | OFF     | —           | `libspiel-dev`                                 | Alternative speech engine (LibSpiel)           |
| `ENABLE_XSLT`               | ON      | 1.1.13      | `libxslt1-dev`                                 | XSLT transformation support                    |
| `ENABLE_INTROSPECTION`      | ON      | —           | `gobject-introspection libgirepository1.0-dev` | GObject introspection                          |
| `ENABLE_DOCUMENTATION`      | ON      | —           | `pip3 install gi-docgen`                       | API documentation generation                   |
| `ENABLE_JOURNALD_LOG`       | ON      | —           | `libsystemd-dev` or `libelogind-dev`           | Systemd journal logging                        |
| `ENABLE_BUBBLEWRAP_SANDBOX` | ON      | —           | `bubblewrap xdg-dbus-proxy libseccomp-dev`     | Process sandboxing (Linux only)                |
| `ENABLE_WEBDRIVER`          | ON      | —           | —                                              | WebDriver automation support                   |
| `ENABLE_PDFJS`              | ON      | —           | —                                              | PDF.js viewer                                  |
| `ENABLE_VIDEO`              | ON      | —           | GStreamer (see prerequisites)                  | HTML5 video support                            |
| `ENABLE_WEB_AUDIO`          | ON      | —           | GStreamer (see prerequisites)                  | Web Audio API support                          |
| `ENABLE_GAMEPAD`            | ON      | 0.2.4       | `libmanette-0.2-dev`                           | Gamepad/controller support                     |
| `ENABLE_MEDIA_STREAM`       | ON      | —           | GStreamer plugins                              | Camera/microphone access                       |
| `USE_GSTREAMER_WEBRTC`      | OFF     | —           | `gstreamer1.0-plugins-bad`                     | GStreamer-based WebRTC                         |

##### Features Disabled by Default (Experimental/Advanced)

| CMake Flag                     | Default | Dependencies (Debian/Ubuntu)                     | Description                                                              |
| ------------------------------ | ------- | ------------------------------------------------ | ------------------------------------------------------------------------ |
| `ENABLE_WPE_PLATFORM`          | OFF     | See platform flags below                         | WPE 2.0 platform abstraction (**required for this plugin**, ⚠️ see note) |
| `ENABLE_WPE_PLATFORM_HEADLESS` | OFF     | See platform flags below                         | Headless platform (**required for this plugin**, ⚠️ see note)            |
| `ENABLE_ENCRYPTED_MEDIA`       | OFF     | Thunder/OCDM                                     | Encrypted Media Extensions (EME/DRM)                                     |
| `ENABLE_WPE_PLATFORM_DRM`      | OFF     | `libinput-dev libudev-dev libdrm-dev libgbm-dev` | DRM/KMS platform (requires `USE_GBM`)                                    |
| `ENABLE_WPE_PLATFORM_WAYLAND`  | OFF     | `libwayland-dev wayland-protocols`               | Wayland platform                                                         |
| `ENABLE_WPE_QT_API`            | OFF     | Qt5/Qt6 development packages                     | Qt/QML API bindings                                                      |
| `USE_QT6`                      | OFF     | `qt6-base-dev qt6-declarative-dev`               | Use Qt6 instead of Qt5 (requires `ENABLE_WPE_PLATFORM`)                  |
| `ENABLE_WPE_1_1_API`           | OFF     | —                                                | Build WPE 1.1 API instead of 2.0                                         |

> **⚠️ WPEPlatform for this Plugin:** The `ENABLE_WPE_PLATFORM` flags above are for **building WPE WebKit from source**. To use the modern WPEPlatform backend with this Flutter plugin (the default in WPE mode), you must enable `ENABLE_WPE_PLATFORM=ON` and `ENABLE_WPE_PLATFORM_HEADLESS=ON` when building WPE WebKit. If these are disabled, the plugin will fall back to WPEBackend-FDO.
