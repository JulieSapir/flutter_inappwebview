## 0.3.0-beta.2

- **修复页面滚动过快**：`GtkSetScrollDelta` 存在双重单位换算——Flutter
  engine（`fl_scrolling_manager.cc`）已将 GDK scroll delta 乘
  `kScrollOffsetMultiplier(53) * scale_factor` 转为物理像素，原样回填
  `GDK_SCROLL_SMOOTH` 事件后被 WebKit 再次按内部步长换算（滚轮一格实测
  ~2120px）。改为 `delta = px / (scale_factor × max(34, H/7))` 归一（
  WebKitGTK SMOOTH 步长按标尺页实测标定：视口 204 高 34px/单位、320 高
  46px/单位，两点拟合 `max(34, H/7)`，±10% 外推不确定性）。终验：滚轮
  一格精确滚动 53px，与 Flutter 桌面语义一致；触控板 `panDelta` 同像素
  语义统一换算
- **修复输入框 caret（光标）不显示**：离屏 popup 宿主永不持有 X toplevel
  focus，`gtk_widget_grab_focus` 只更新 GTK 内部 focus widget 状态，不会向
  WebKitWebView 投递 `GDK_FOCUS_CHANGE`（focus-in 事件实测零到达），WebKit
  的 ViewIsFocused 拉不起来 → caret 不绘制；键盘事件不走 focus 通道所以
  输入仍有效，形成"能输入但无光标"。新增 `GtkSetFocused` 显式合成 focus
  事件补链路（与其他合成输入同模式，WebKitGTK 内部 isFocused 幂等）；
  `is_focused_` 初始值 true→false 对齐 WebKit 实际状态（避免首个
  setFocused(true) 被幂等保护吞掉）。终验：点击 input 出现 focus 高亮与
  闪烁 caret，文字输入正常
- 删除死枚举 `WpePointerButton`（零使用，且 `None` 与 X11/X.h 的
  `#define None 0L` 宏冲突，链路重编时报 expected identifier）
- 新增 debug 取证打点：WebKit focus-in/out 事件与 `setFocused` 调用日志
  （仅 debug 构建），供焦点链路问题排查
- example 新增测试资产：`scroll_ruler.html`（40px 刻度滚动标尺）、
  `input_focus_test.html`（focus/caret 测试页）

## 0.3.0-beta.1

- **移除 WPE WebKit 后端**：WebKitGTK 4.1 成为唯一后端。删除
  `HAVE_WPE_PLATFORM` / `HAVE_WPE_BACKEND_LEGACY` 全部代码路径
  （WPEPlatform/FDO 初始化、DMA-BUF/SHM 缓冲导出、backend resize 分支、
  C 风格导出回调、WPEPlatform 设置应用、WPE 专属纹理类引用）；
  CMake 移除后端开关与 WPE 依赖（wpe-webkit/libwpe/wayland-server），
  仅保留 webkit2gtk-4.1（REQUIRED）
- **移除 snapshot CPU 软渲染回退**：GPU 直通（XComposite + Damage +
  EGLImage 零拷贝）成为唯一渲染管线。删除
  `webkit_web_view_get_snapshot` 纹理管线（GtkOffscreenWindow 宿主、
  50ms 节拍器、三缓冲 pixel_buffers、SIMD BGRA→RGBA 转换、
  FlPixelBufferTexture）；能力检查不满足时显式报错（errorLog），不做
  静默回退。`takeScreenshot` 改走 `webkit_web_view_get_snapshot` 异步
  API（与纹理管线解耦，GPU 直通下同样可用）
- `webkit_include.h` 收敛层重建：WebKitNetworkSession→WebKitWebContext、
  WebKitRectangle→GdkRectangle、WebKitColor→GdkRGBA、
  web_context_set_web_extensions_directory 命名差异（webkit2gtk-4.1 2.52 实测）
- `InAppBrowser` 移除 GtkGLArea/GtkDrawingArea 渲染中转层残留（WPE 时代
  像素/纹理回传路径），webview widget 直接挂浏览器窗口
- 移除 WPE 时代的 VM 软渲染预检（`LIBGL_ALWAYS_SOFTWARE` 自动注入），
  GPU 能力由 `WebKitGpuCapture::IsSupported` 显式探测
- 实测回归（:0，GPU 直通，webkit2gtk-4.1 2.52.3）：编译通过；GPU capture
  激活、首帧 `repaint-complete` 干净交付；10 步 resize 步进 present size
  全程跟踪、终态全宽渲染无脏块；右键菜单正常弹出、多次操作零 SIGSEGV；
  fps 打点正常（页面动画率 35-49fps）

## 0.2.0-beta.3

- 修复初次载入时 webview 显示未初始化显存噪声（脏帧）：
  - 守门「重绘完成」判定从「单批 damage 包围盒覆盖全 drawable」扩展为
    **跨批累计 damage 并集覆盖**——WebKit 初次载入是渐进分块渲染，单批
    判定永远凑不齐，此前只能等 250ms 超时放行部分内容帧（脏）
  - **首帧零超时兜底**：从未向引擎交付过帧时（初次载入），必须等到
    WebKit 重绘完成才放行。RCA：构造初始尺寸阶段（如 800x600）几何正确
    但 WebKit 尚未绘制，backing 全是未初始化显存，任何超时放行都是脏帧；
    极端场景（WebProcess 永不绘制）黑屏本身即真实状态。实测首帧在
    65~700ms 内以 `repaint-complete` 干净交付，稳定复现 3 轮 +
    resize 重建实例场景从 5s 超时脏帧变为 ~390ms 干净交付
  - resize 过渡期（已有旧帧）250ms 兜底保留不变
- GPU 直通模式下 popup 宿主 map 后立即用 `gdk_window_move_resize` 钉位
  目标几何（InitGtkHost），加快构造尺寸到目标尺寸的收敛

## 0.2.0-beta.2

- 修复调整窗口宽高时渲染冻结（拖拽卡顿）：GPU 直通守门对每次几何变化
  都重置等待状态，WebKit 全幅重绘（~60-100ms）赶不上连续步进（~65ms），
  在途重绘 damage 被下一步重置吞掉，实测 20 步拖拽 1.2s 零帧交付（画面
  完全静止）。改为 resize 过渡时用旧帧内容 + 边缘像素色填充新 backing
  （`XCopyArea` 重叠区 + `XFillRectangle` 增量区，深/浅色页面自适应）后
  **立即放行**：backing 全域有效（无未初始化显存），每步即时出帧，
  WebKit 全幅重绘随后作为稳态帧覆盖。实测 20 步拖拽每步出帧、无 X 错误
- 修复 GPU 直通守门丢弃的 `NameWindowPixmap` 别名泄漏（等待路径每步泄漏
  一个 pixmap，拖拽场景放大显存压力）
- 修复初次载入时 webview 显示未初始化显存噪声（脏帧）：
  - 守门「重绘完成」判定从「单批 damage 包围盒覆盖全 drawable」扩展为
    **跨批累计 damage 并集覆盖**——WebKit 初次载入是渐进分块渲染，单批
    判定永远凑不齐，此前只能等 250ms 超时放行部分内容帧（脏）
  - **首帧零超时兜底**：从未向引擎交付过帧时（初次载入），必须等到
    WebKit 重绘完成才放行。RCA：构造初始尺寸阶段（如 800x600）几何正确
    但 WebKit 尚未绘制，backing 全是未初始化显存，任何超时放行都是脏帧；
    极端场景（WebProcess 永不绘制）黑屏本身即真实状态。实测首帧在
    65~700ms 内以 `repaint-complete` 干净交付（3 轮重启稳定复现），
    resize 重建实例场景从 5s 超时脏帧变为 ~390ms 干净交付
  - resize 过渡期（已有旧帧）250ms 兜底保留不变
- GPU 直通模式下 popup 宿主 map 后立即用 `gdk_window_move_resize` 钉位
  目标几何（InitGtkHost），加快构造尺寸到目标尺寸的收敛
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
