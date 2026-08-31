## 0.2.0-beta.2

- 修复右键菜单 segfault：WebKitGTK 4.1 的 `context-menu` 信号自 2.40 起在
  `context_menu` 与 `hit_test_result` 之间插入了 `GdkEvent*` 参数，旧 4 参回调
  导致 `hit_test_result` 被读进 `user_data` 槽（`self` 悬空），右键即崩溃。
  `OnContextMenu` 在 `HAVE_WEBKIT_GTK` 分支下对齐新签名
  (`in_app_webview.cc`/`in_app_webview.h`)
- 修复 `<select>` 下拉菜单 segfault：同源问题，`OnShowOptionMenu` 的
  `show-option-menu` 信号同样带 `GdkEvent*` 参数，一并补齐
- WPE 旧分支保留 2 参签名（WPE1 2.40-2.50 契约；WPE1 2.52 起同样插入 event
  参数，随 WPE 后端下线一并清理）

## 0.2.0-beta.1

**BREAKING**: Linux 默认后端从 WPE WebKit 切换为 **WebKitGTK 4.1**（Tauri 2 同款，发行版官方包 `libwebkit2gtk-4.1-dev` 可直接安装，无需自编译）。

- 新增 `FLUTTER_INAPPWEBVIEW_LINUX_BACKEND` CMake 开关：`gtk`（默认）/ `wpe`（保留为可选，WPE 用户需显式开启且自备 WPE 库）
- 新增 WebKitGTK 渲染管线：`webkit_web_view_get_snapshot()` → SIMD BGRA→RGBA → 共享三缓冲 → `FlPixelBufferTexture`（`in_app_webview_gtk.cc`）
- 帧驱动：50ms 节拍器 + `snapshot_pending_` 防重入（WebKitGTK 无 per-frame 回调）
- 输入：Flutter 指针/滚轮/键盘事件合成 GdkEvent 投递（`GtkSetCursorPos/GtkSetPointerButton/GtkSetScrollDelta/GtkSendKeyEvent`）
- InAppBrowser：webview widget 直接挂载浏览器窗口，原生渲染/输入/IME（替换原 GtkGLArea WPE 纹理中转）
- Headless：`GtkOffscreenWindow` 离屏宿主
- 修复 `onReceivedError` 的 Dart 崩溃：`WebResourceErrorType` 无 Linux native value 映射导致 `fromNativeValue` 返回 null（存量 bug，WPE 模式同样存在）；Linux 端改为直接构造，type 降级为 `IO`，完整信息保留在 description
- 已知限制：`SendTouchEvent` 不支持（GdkEventSequence 无法合成）；`requestPointerLock/Unlock` 返回 false；`itpEnabled` 无 API 支持
- WPE 后端代码路径保留（`HAVE_WPE_PLATFORM` / `HAVE_WPE_BACKEND_LEGACY` 宏），下个 minor 视使用情况移除
- 详见 `LINUX_BACKEND.md`

## 0.1.0-beta.1

- Initial release.
