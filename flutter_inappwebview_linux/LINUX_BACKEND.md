# Linux 后端文档

flutter_inappwebview_linux 使用唯一后端 **WebKitGTK（webkit2gtk-4.1）**。

## 为什么用 WebKitGTK

- **发行版官方包**：`apt install libwebkit2gtk-4.1-dev` 即可（Ubuntu 22.04+ / Debian 12+ / Fedora / Arch）
- 与 Tauri 2 相同的 WebKit 移植，兼容性长期有保障

## 架构说明

| 能力                     | 实现方式                                                                                                                                                                   |
| ------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| InAppWebView widget 渲染 | **GPU 直通（唯一管线）**：XComposite redirect + Damage 驱动 + `NameWindowPixmap` 别名 + `EGL_KHR_image_pixmap` 零拷贝导入引擎纹理。无 CPU 软渲染回退，能力不满足时显式报错 |
| 帧驱动                   | XDamage 事件驱动（页面变才出帧，实测跟随页面更新率直至刷新率上限）；resize/scale 变化后由 `PresentOnce` 强制补帧                                                           |
| 输入                     | Flutter 指针/滚轮/键盘事件合成 GdkEvent → `gtk_widget_event()`                                                                                                             |
| 离屏宿主                 | override-redirect popup 定位屏内锚点（origin=0 显示器右下角内侧 1x1；需真实 X 窗口供捕获）。屏内定位保证 `window.screenX/screenY` 语义正确——屏外负坐标会让 testufo 等站点误判"窗口不在主显示器"报 SYNC FAILURE。隐形：XShape bounding/input 全空（直接输出路径零像素；muffin 类合成器无视 shape，最坏绘制 1px） |
| InAppBrowser             | webview widget 直接挂入浏览器窗口（原生渲染/输入/IME，无纹理中转）                                                                                                         |
| Headless                 | 复用离屏宿主，不注册纹理                                                                                                                                                   |

### GPU 直通实测（i915 / 60Hz / flutter.dev / 1280x204）

| 指标                | GPU 直通                                       |
| ------------------- | ---------------------------------------------- |
| 滚动帧率            | **60fps**（打满刷新率）                        |
| 滚动 CPU（flutter） | 0.4%                                           |
| 静态页 idle         | 零 present（零功耗）                           |
| 动画页 idle         | 跟随页面变化率（如 52fps），不多渲染也不欠采样 |

GPU 直通启用条件（`WebKitGpuCapture::IsSupported`）：原生 X11 + XComposite/XDamage 扩展 + `EGL_KHR_image_pixmap`。不满足时（如 Xvfb 无 DRI3）应用照常运行但 webview 无画面，并输出 errorLog（不做静默回退）。XWayland 下三项条件均满足（DRI3 由 mesa 提供），GPU 直通预期可用。

### 已知限制（显式声明）

- `SendTouchEvent` 不支持（GTK3 无法合成 `GdkEventSequence`）
- `requestPointerLock/Unlock` 返回 false（WebKitGTK 无公开 API）
- ITP（`itpEnabled`）无 API 支持（显式报错）
- `WebResourceErrorType` 契约层无 Linux native value 映射（platform_interface 缺口），onReceivedError 的 error.type 降级为 IO，完整信息在 description

### 安装与使用

```bash
sudo apt-get install -y libwebkit2gtk-4.1-dev
flutter build linux --debug
```

### 修订记录

- 2026-09：移除 WPE 后端与 snapshot CPU 软渲染回退，GPU 直通成为唯一渲染管线（能力不满足显式报错）；`takeScreenshot` 改走 `webkit_web_view_get_snapshot` 异步 API，与纹理管线解耦
- 2026-08：GPU 直通管线成为默认（XComposite+Damage+EGLImage 零拷贝，实测帧率 3.2×）；present 稳态零 X 往返（尺寸缓存 scale-aware）；fps debug 打点；snapshot 降级为回退路径
- 2026-08：新增 WebKitGTK 后端；渲染/输入适配实现在 `in_app_webview_gtk.cc`，API 差异收敛在 `webkit_include.h`（NetworkSession→WebContext、WebKitRectangle→GdkRectangle、WebKitColor→GdkRGBA 等）
