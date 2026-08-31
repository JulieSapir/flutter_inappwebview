## 0.2.0-beta.2

- 新增 **GPU 直通渲染管线**（默认，能力探测自动启用）：XComposite redirect +
  Damage 驱动 + `XCompositeNameWindowPixmap` 别名 + `EGL_KHR_image_pixmap`
  零拷贝导入引擎纹理（`webkit_gpu_capture.cc`）。实测（i915 / 60Hz /
  flutter.dev / 1280x204）：滚动帧率 19fps → **60fps（3.2×）**，CPU 持平
  （≤0.4%）；静态页 idle 零 present；动画页跟随页面实际变化率。不满足条件
  （Wayland / Xvfb / 无 EGL pixmap import）自动回退 snapshot 管线；
  `FLUTTER_INAPPWEBVIEW_LINUX_GPU_CAPTURE=0` 可强制回退
- GPU present 稳态零 X 往返：尺寸直接取 damage 事件自带的 drawable 几何
  （服务端真值，与该帧 backing 内容严格同源）；接线补首帧/resize 强制补帧时
  才退化为单次 `XGetGeometry`。修复 resize 时画面拉伸/花屏（原 allocation
  缓存推断与 X 服务端 resize 分属两条连接非原子，过渡期旧尺寸配新 pixmap）
- 修复 resize 时 popup 宿主 X 窗口卡旧尺寸导致的内容裁切 + 未初始化显存
  噪声（`gtk_window_resize` 与手动 `size_allocate` 竞态跳过 `XResizeWindow`，
  X11 子窗口被祖先裁剪）：popup 宿主改走 `gdk_window_move_resize` 单写入者
  确定生效，并随尺寸重钉屏外定位；`gtk_window_resize` 仅保留给
  GtkOffscreenWindow（snapshot 路径）
- 修复拖拽过程中脏区域被渲染：几何变化后跳过服务端 resize 自带的全幅
  damage 帧（此时 backing 仅旧尺寸区域有效），等 WebKit 全幅重绘（damage
  包围盒覆盖全 drawable）再 present，250ms 兜底；期间引擎沿用旧帧
- 新增尺寸变化打点（debug）：`present size WxH`，resize 诊断与 damage 几何
  真值实证；fps 打点统计守门后真实交付帧
- 新增 fps debug 打点（每 5s 惰性窗口）：GPU `present fps=N` / snapshot
  `snapshot fps=N`，供 `scripts/bench_render.sh` 基准对比
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
