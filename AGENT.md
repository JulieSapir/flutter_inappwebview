# AGENT.md — flutter_inappwebview 维护日志

> 本文件由维护 agent 写入，供下次接手时快速恢复上下文。

## 2026-08-31 交付：Linux 后端迁移 WPE → WebKitGTK 4.1

### 需求背景

用户痛点：WPE WebKit 无发行版官方包（30+ 依赖自编译），不可维护。切换到 WebKitGTK 4.1（Tauri 2 同款，`apt install libwebkit2gtk-4.1-dev` 即装）。

### 完成内容

1. **CMake 后端开关**（`flutter_inappwebview_linux/linux/CMakeLists.txt`）
   - `FLUTTER_INAPPWEBVIEW_LINUX_BACKEND`：`gtk`（默认，探测 `webkit2gtk-4.1`，定义 `HAVE_WEBKIT_GTK=1`）/ `wpe`（原探测逻辑整体保留）
   - WPE 专属源文件（`inappwebview_egl_texture.cc`）与 GTK 专属源文件（`in_app_webview_gtk.cc`）按开关条件编译；WPE 库打包逻辑仅在 wpe 模式生效
2. **API 差异收敛层**（`linux/webkit_include.h`，新文件）
   - 所有 WPE/GTK 共享代码统一 include 此头。已核实的差异（以本机 webkit2gtk-4.1 2.52.3 头文件为权威）：
     - `WebKitNetworkSession`（WPE 2.40+）→ GTK 用 `WebKitWebContext`（宏重定向：get_default/new_ephemeral/get_cookie_manager/get_website_data_manager/set_proxy_settings/allow_tls_certificate_for_host/`webkit_web_view_get_network_session`→`webkit_web_view_get_context`）
     - `WebKitRectangle`→`GdkRectangle`、`WebKitColor`→`GdkRGBA`
     - GTK 2.52 仍是 `webkit_web_context_set_web_extensions_directory`（非 web*process*）
     - snapshot API：GTK 为 `webkit_web_view_get_snapshot(view, region, options, cancellable, cb, data)` 老签名（无宽高参数）；WPE 2.40+ 为 `webkit_web_view_snapshot(view, w, h, ...)`
     - register/unregister_script_message_handler：GTK 老签名 2 参（包装函数 `webkit_compat_register_script_message_handler`）
     - `gtk_web_view_new()` GTK 返回 `GtkWidget*`（WPE 返回 `WebKitWebView*`），需显式 `WEBKIT_WEB_VIEW()` 转型
3. **渲染管线**（`linux/in_app_webview/in_app_webview_gtk.cc`，新文件）
   - `GtkOffscreenWindow` 离屏宿主 → `webkit_web_view_get_snapshot`（VISIBLE）→ cairo ARGB32 → `ConvertARGB32ToRGBA`（simd）→ 复用 WPE 的三缓冲 `pixel_buffers_` → 既有 `FlPixelBufferTexture`
   - 帧驱动：50ms `g_timeout` 节拍器 + `snapshot_pending_` 防重入（WebKitGTK 无 per-frame 回调）
   - **关键实证**（spike）：`GtkOffscreenWindow` 下仅 `gtk_window_resize` 时 widget allocation 仍为 1x1（snapshot 出图 1x1），必须手动 `gtk_widget_size_allocate` 强制同步
4. **输入桥**（in_app_webview_gtk.cc）：Flutter 指针/滚轮/键盘 → 合成 GdkEvent（`DartModifiersToGdk` 修饰键位序转换 C=1,S=2,A=4,M=8 → GDK 掩码）→ `gtk_widget_event`。触摸不支持（GdkEventSequence 无法合成，显式 errorLog）
5. **InAppBrowser**：`hostInBrowserWindow=true` 跳过离屏宿主，webview widget 直接挂浏览器窗口（原生渲染/输入/IME），替换原 GtkGLArea WPE 纹理中转
6. **存量 bug 修复**（`flutter_inappwebview_linux/lib/src/in_app_webview/in_app_webview_controller.dart` onReceivedError case）：`WebResourceErrorType` 无 Linux native value 映射 → `fromNativeValue(...)!` 恒崩（WPE 模式同样存在）。改为直接构造 WebResourceError，type 降级 `IO`，description 保留完整信息。**根治需 platform_interface 契约补 Linux 值映射**（待办）
7. 文档：`LINUX_BACKEND.md`（新）、`CHANGELOG.md` 0.2.0-beta.1

### 验证状态（本机：webkit2gtk-4.1 2.52.3 / GTK 3.24.41 / Flutter 3.44.9 / X11）

- `flutter build linux --debug` 通过
- example 运行：主 webview 纹理渲染正常（截屏确认页面内容显示），snapshot 失败仅启动期 1 次（自动重试成功），无 Dart 异常
- `dart analyze`：本次改动文件无新增 issue（存量 override warning 与 2024 个 info 与本次无关）

### 本地验证 SOP（path 依赖开关）

仓库 pubspec 默认走 pub 版本（发布态）。本地验证时：

```bash
# 1. 打开 path 依赖：7 个 pubspec 中把依赖行改为
#    flutter_inappwebview_platform_interface:
#      path: ../flutter_inappwebview_platform_interface
#    （主包 6 个联邦依赖 + 各平台包内部的 platform_interface 依赖都要改，否则解析冲突）
# 2. flutter pub get（主包与 example）
# 3. flutter_inappwebview/example 下 flutter build linux --debug
# 4. 运行 ./build/linux/x64/debug/bundle/flutter_inappwebview_example
# 5. 提交前把 path 依赖还原为注释态
```

### 遗留事项（下次接手）

- [ ] **IME 中文输入验证**：GdkEvent 合成路径下 `gtk_im_context` 行为未验证（P0 风险，需真实中文输入法环境）
- [ ] **性能 benchmark**：1080p/1440p 滚动场景 CPU 占用、节拍器动态周期（`webkit_web_view_is_loading` 切换 33ms/100ms）
- [ ] **InAppBrowser 运行时验证**：GTK 挂载路径编译通过但未跑 example 的 InAppBrowser 页面
- [ ] **Headless 运行时验证**：无显示环境（CI/Xvfb）下 `GtkOffscreenWindow` + snapshot 行为
- [ ] **触摸注入**：调研 GTK3 `gdk_offscreen_window` 序列合成或升级 GTK4（webkit2gtk-6.0）后重评
- [ ] **platform_interface 契约**：`WebResourceErrorType` 补 Linux native value 映射（需改生成器注解模板）
- [ ] **CI**：补 Linux job（apt 装 libwebkit2gtk-4.1-dev 后编译门禁）
- [ ] WPE 代码路径（`HAVE_WPE_PLATFORM`/`HAVE_WPE_BACKEND_LEGACY` 宏）计划下个 minor 移除，届时删 `inappwebview_egl_texture.cc`、`in_app_webview.cc` 的 WPE 分支、`LINUX_BACKEND.md` WPE 章节
- [ ] 仓库根 README 的 Linux 安装说明指向 `LINUX_BACKEND.md`

### 环境备注

- 本机 webkit2gtk-4.1 头文件为 2.52 扁平布局：聚合头 include `<webkit/WebKit*.h>`（非 webkit2/ 子目录）
- WebKitGTK 2.52 的 4.1 API 面是 WebContext 模型（无 NetworkSession），与 WPE 2.40+ 的 NetworkSession 模型不同
- 长命令一律 `timeout` 限时；root 下 flutter 会打印警告（无害）
