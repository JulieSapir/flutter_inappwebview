# AGENT.md — flutter_inappwebview 维护日志

> 本文件由维护 agent 写入，供下次接手时快速恢复上下文。

## 2026-08-31 修复：拖拽 resize 冻结（每步几何变化重置等待 → 20 步拖拽 1.2s 零帧）

### RCA（present size 时间线实证）

- 症状：调整窗口宽高时画面卡顿/冻结，拖拽停止后才恢复
- 日志铁证（修复前）：20 步连续 resize（65ms 间隔）期间**零帧交付**，
  只有拖拽停止后尾部 1 帧（`repaint-complete after 387ms`）。机制：
  每步几何变化都进入 await 等待并重置 damage 并集累计 + 250ms 兜底计时，
  WebKit 全幅重绘（~60-100ms）赶不上下一步（65ms），在途重绘 damage 到达时
  几何已再变（新世代）→ 被重置吞掉 → 步进期间永远凑不满 → 全程冻结
- 判据演进（中间方案被实测否决一次）：先做「XCopyArea 旧帧重叠区 +
  effective 并集判定」——缩小方向立即放行 ✓，但放大方向仍要等 WebKit 补画
  增量区，重绘赶不上步进时依旧零帧（日志复现）。最终改为「填充后立即放行」
- 顺手修复：守门等待路径丢弃的 `NameWindowPixmap` 别名未释放（每次泄漏一个
  pixmap，拖拽场景放大显存压力）

### 最终方案（`webkit_gpu_capture.cc` 守门重构）

resize 过渡（有旧帧）时把新 backing **填满后立即放行，零等待**：

1. `XCopyArea`：旧帧（bound 优先，pending 兜底，锁内快照锁外调用）左上
   重叠区 → 新 backing（缩小方向即全域覆盖）
2. `XFillRectangle`：增量区（右条 + 下条 L 形）用旧帧右下角单像素色填充
   （`XGetImage` 1x1 取色，深/浅色页面自适应，失败回退白色）
3. backing 全域有效（无未初始化显存）→ 立即 present；WebKit 全幅重绘
   ~60-100ms 后到达，作为稳态帧覆盖（present 后 awaiting=false 直通道）

首帧场景（无任何已交付帧）逻辑不变：跨批 damage 并集凑满才放行、零超时
兜底（黑屏优于噪声）。拷贝/填充 X 错误时回退等待 + 250ms 兜底。

### 验证（:0，GPU 直通，重建后二进制）

- 20 步拖拽（65ms 间隔，900↔1530 往返）：**每步即时出帧**（present size
  全程跟踪 990→1170→1350→…→1260，20/20），无 await 阻塞、无 fill failed、
  零 X 错误；修复前同场景 1.2s 零帧
- 视觉：窗口 activate 后 1550x720 截图——webview 全宽渲染 flutter.dev
  完整内容，无噪声/脏块/花屏
- 回归：初次载入首帧仍 `repaint-complete` 干净帧（117ms，800x600 阶段零
  放行）；稳态 fps=19 与基线一致

### 教训

- **「等待重绘完成」类守门必须回答「步进中怎么办」**：任何以「完整性」为
  放行条件的机制，在连续变化场景下都会被下一步重置清零——要么填满内容
  立即放行（本次方案），要么接受冻结。中间态（等增量重绘）两头不讨好
- X11 backing 无「已绘制」元数据，但可以**自己制造有效性**：拷贝旧内容 +
  纯色填充增量区，把「等待 WebKit」转化为「已知有效内容立即交付」

## 2026-08-31 修复：初次载入脏帧（渐进渲染凑不齐单批全幅 + 构造尺寸阶段 backing 未初始化）

### RCA（日志诊断打点实证，三轮判据修正）

- 症状：webview 初次载入时画面是脏的（未初始化显存噪声），加载完成后自愈
- 诊断手段：PresentOnce 守门逻辑加 debug 打点（`await full repaint WxH` /
  `release frame TIMEOUT|repaint-complete ... (batch_full= union_full= old_frame=)`），
  日志取证不依赖窗口截图（:0 桌面被 Minecraft 占用时截图法不可行）
- 日志铁证（修复前）：初次载入两次放行都是 `TIMEOUT + covers_full=0`——
  ① 800x600 阶段（构造初始尺寸，GTK 布局轮 0.2~1.2s 后才把 X 窗口调到目标
  1280x204）：WebKit 尚未绘制，backing 全是未初始化显存，超时放行=脏帧
  ② 1280x204 阶段：WebKit 渐进分块渲染，每批 damage 只覆盖一部分，
  「单批包围盒覆盖全 drawable」永远凑不齐 → 250ms 超时放行部分内容帧（脏）
- 判据修正过程（两个中间方案被实测否决）：
  - 方案 A「并集覆盖 + 首帧 5s 兜底」：resize 步进中创建的实例（example 重建
    widget）页面已稳定、后续只有局部动画 damage，并集永凑不满 → 5s 黑屏
  - 方案 B「首帧 1s 兜底 + expected 几何校验」：几何收敛时机运行间波动
    （164ms~1.2s），慢收敛时 1s 兜底先触发，800x600 脏帧复现；且 800x600 是
    「当时的正确几何」（Dart 稍后才 setSize），expected 校验方向本身就错
  - 最终方案（生效）：**首帧零超时兜底**——从未向引擎交付过帧时必须等到
    重绘完成（单批或渐进并集覆盖）才放行；resize 过渡（已有旧帧）保留 250ms
    兜底。极端场景（WebProcess 永不绘制）黑屏即真实状态，优于显存噪声
- 附带发现：example 主窗口 resize 会销毁重建 webview widget（应用行为），
  新实例以构造尺寸 800x600 创建后立即经历连续 resize 步进——该场景旧逻辑
  5s 超时脏帧、新逻辑 387ms `repaint-complete` 干净交付

### 修复内容（`webkit_gpu_capture.{h,cc}`、`in_app_webview_gtk.cc`）

1. 守门「重绘完成」判定：DamageSource 回调传出本批 damage bbox
   （`on_damage(w,h,covers_full,min_x,min_y,max_x,max_y)`），PresentOnce 在
   await 期间跨批累计并集（`await_bbox_*`，进入等待/几何变化时重置），
   并集覆盖全 drawable 即放行；250ms 兜底仅对「已有旧帧」的 resize 过渡生效
2. InitGtkHost：popup 宿主 map 后立即 `gdk_window_move_resize` 钉位目标几何
   （同 setSize 的单写入者原则），加快构造尺寸→目标尺寸收敛（include 补
   `gdk/gdkx.h`，`GDK_IS_X11_WINDOW`）
3. 打点保留：`release frame` 日志含 batch_full/union_full/old_frame 三元组，
   供下次渲染问题排查对照

### 验证（:0，GPU 直通，重建后二进制）

- 3 轮重启复验：首个交付帧均为 `repaint-complete` 1280x204（65/96/113ms），
  800x600 阶段零放行，零 TIMEOUT
- resize 步进回归（8 步连续 resize）：重建实例 387ms 干净交付；稳态 fps=19
  （页面动画实际变化率）与修复前一致
- 遗留：`g_object_unref` CRITICAL（secondary webview 销毁路径，存量问题）
  本次仍出现一次，与本次改动无关

### 教训

- **「超时兜底放行」对首帧场景是脏帧发生器**：兜底只适用于「有旧帧可退」的
  场景；首帧的退路（黑屏）与脏帧的取舍必须显式选择
- **X backing 内容无「已绘制」元数据**：damage 事件的 area/geometry 无法区分
  「WebKit 真实绘制」与「窗口配置杂项 damage」，只能靠守门策略取舍
- 守门类日志（进入等待/放行原因）值得长期保留，是渲染时序问题的第一取证手段

## 2026-08-31 修复：resize 渲染异常（popup 宿主 X 窗口卡旧尺寸 → X11 祖先裁剪）

### 完整 RCA（两轮修复，实证闭环）

- 症状：调整窗口 wh 时 GPU 直通画面异常——内容 1:1 保留在左上旧尺寸区域，
  右侧/下方为未初始化显存噪声（非拉伸、非花屏、非黑屏），且**卡死不自愈**
- 第一轮修复（`webkit_gpu_capture.cc`，必要但不充分）：尺寸缓存用 GTK
  allocation 推断 X 真值，跨连接非原子 → 改为直接取 damage 事件自带的
  drawable 几何（`XDamageNotifyEvent.geometry`，与该帧 backing 严格同源，
  稳态零往返）；接线补首帧/resize 强制补帧才退化单次 `XGetGeometry`。
  `Impl.cached_w/h`、`Impl.widget` 整删，`DamageSource.on_damage` 签名 `(w,h)`
- 第二轮（**真根因**，`in_app_webview.cc` `setSize`）：现场取证窗口树发现
  **popup 宿主 X 窗口卡在旧尺寸**（1280x204）而 webview 子窗口已 1597x320。
  X11 子窗口渲染被祖先裁剪 → 超出宿主旧尺寸的内容不落地，backing 其余区域
  是未初始化显存。判别实验：外部 `xdotool windowsize` 把宿主改到 1597x320，
  **画面瞬间痊愈**且页面早已按新尺寸重排完毕（纯裁剪问题，WebKit 渲染管线
  全程正常）。竞态机制：`setSize` 里 `gtk_window_resize`（GTK 异步 size 机器）
  与手动 `gtk_widget_size_allocate`（同步）并发——GTK 见 allocation 已等于
  请求值会跳过 `XResizeWindow`；离散步进 resize 概率躲过，交互式拖拽必现
- 修复（单写入者原则）：GPU 直通 popup 宿主完全走 gdk 路径——
  `gdk_window_move_resize(host_gdk, -(2*w+256), -(2*h+256), w, h)` 确定生效
  并顺带重钉屏外定位（尺寸变大右下角可能进屏）；`gtk_window_resize` 只留给
  GtkOffscreenWindow（snapshot 路径，无 X 窗口语义）。子窗口传导继续靠手动
  `size_allocate`（该链路实测每步都正确）
- 排查误区记录：日志中零星 `present size 800x600` 是 example 页面**其他
  webview 实例的启动 present**（每个实例独立 capture、同名日志），不是主
  webview 瞬态；2 条 `GLib-GObject-CRITICAL g_object_unref` 断言来自
  secondary webview 销毁路径（存量问题，与 resize 无关，见遗留）
- 第三轮（拖拽中脏块）：静止已干净后，拖拽过程仍有脏区域——服务端 resize
  会给重分配后的 backing 发一发全幅 damage，这帧内容只有旧尺寸区域有效，
  直接 present 就是脏块。修复：几何变化后进入等待，跳过该帧，等 WebKit 对
  新尺寸的重绘（本批 damage 包围盒覆盖全 drawable，`XDamageNotifyEvent.area`
  累计）再放行；拖拽连续步进几何再变则刷新目标；250ms 兜底防永久卡旧帧。
  **实现教训：等待期间 pending 仍为旧值，「几何≠pending」每帧都成立，
  必须用 await_w/h 判定是否已在等同一目标尺寸，否则全幅重绘帧永远进不了
  放行分支（首版实测死锁：首帧永无落地，no frame yet 刷屏）**。fps 打点
  随之移到守门之后（统计真实交付帧）
- 教训：**不要用 GTK 侧状态推断 X 服务端状态**；同一 X 窗口只能有一个写入者；
  "内容被裁切 + 噪声"先查 X 窗口树的父/子尺寸同步

### 验证（:0，GPU 直通，重建后二进制）

- 拖拽模拟：36+ 步快速连续 resize（80ms 间隔，放大→缩小→再放大），窗口树
  宿主/子窗口全程同步（终态 1500x320/1500x320，重钉 -3256=-(2\*1500+256) ✓）
- 拖拽中途连拍：放大过程中 webview 全宽渲染正确（无脏块/噪声/黑带），
  内容按当前尺寸正确布局；`present size` 全程跟踪，fps 打点统计守门后
  真实交付帧
- 最大化 2560x1400：webview 2560x320 全宽渲染正确（banner 打满、nav 位置
  正确），popup 重钉 -5376 仍在屏外且未越 16-bit 坐标界
- `present size` 每步跟踪（…1450x320→1500x320→2560x320）；无 X 错误、无
  EGL 导入失败；snapshot 回退路径未触碰（`GTK_IS_OFFSCREEN_WINDOW` 分支保持
  原行为）
- 遗留：secondary webview 销毁时 `g_object_unref` 断言（GLib-CRITICAL，非
  致命）待查；example 的隐藏 webview 实例的 popup 宿主也吃同一修复路径

## 2026-08-31 交付：渲染性能量化（GPU 直通 3.2× 帧率实证）+ present 稳态零往返

### 本轮代码改动（`flutter_inappwebview_linux/linux/in_app_webview/`）

1. `webkit_gpu_capture.cc` `PresentOnce` 稳态零 X 往返：原实现每 present 打一发
   `XGetGeometry` 只为拿 w/h（125Hz 节流下纯浪费）。改为 `Impl.cached_w/h` 尺寸
   缓存，仅当 GTK allocation×scale 与缓存不符才向服务端核对。HiDPI scale-aware
   （allocation 是 app 像素、X 窗口是设备像素，×scale 比较）；resize 过渡期自纠偏
   （服务端未追平前缓存≠allocation，持续核对直至一致）。`Start` 记住 widget 指针
2. fps debug 打点（每 5s 惰性窗口，无额外定时器）：
   - GPU：`WebKitGpuCapture: present fps=N`（PresentOnce 内，节流后计数）
   - snapshot：`InAppWebView(gtk): snapshot fps=N`（DeliverSnapshot 内）
3. `scripts/bench_render.sh`（新）：可复现基准脚本。分进程 CPU（/proc stat）+
   fps 汇总；`BENCH_AB=1` 跑 :0 同屏 A/B；`BENCH_GDK_SCALE=2` 支持 HiDPI 压测

### 实测数据（:0 同屏 A/B，i915 硬件 / 60Hz / flutter.dev / webview 1280x204）

| 指标                          | GPU 直通                    | snapshot 回退                                         |
| ----------------------------- | --------------------------- | ----------------------------------------------------- |
| 滚动帧率                      | **60fps**（打满刷新率）     | **19fps**（50ms 节拍器上限钉死）                      |
| 滚动 CPU（flutter 进程）      | 0.4%                        | 0.2-0.4%（持平）                                      |
| 静态页 idle                   | 零 present（零功耗）        | ~20fps 节拍器永动机（读回+转换+上传）                 |
| 动画页 idle                   | 跟随页面实际变化率（52fps） | 仍 19fps（**欠采样**，页面 52fps 在变用户只见 19fps） |
| HiDPI GDK_SCALE=2（2560x408） | 0.3% CPU，零拷贝保持        | 0.3%/0.7%（读回放大 4×）                              |

- 关键方法论：**CPU% 在桌面硬件+小帧面上无区分度**（两条管线全 ≤0.4%），帧率才是
  正确仪器。架构优势随帧面面积、页面 damage 量放大；GPU 管线 idle 出帧=页面变化率
  （不多渲染一帧也不漏采样），snapshot idle 恒 20fps 与页面状态无关
- 回归：改动前后 :99 snapshot 管线 idle 1.2%/0%、scroll 1.1%/0.1% 同量级，无退化

### 基准环境 SOP（新增，避坑实证）

- **Xvfb 虚拟显示**（`Xvfb :99 -screen 0 1600x1000x24`）可跑 snapshot 回归与日常
  验证，**完全不占前台**（用户可继续用桌面/打游戏）。但 Xvfb 无 DRI3 → EGL probe
  `EGL_KHR_image_pixmap unavailable` → GPU 直通必然回退 snapshot（`LIBGL_ALWAYS_SOFTWARE=1`
  实测也无效）→ **fps/帧率 A/B 必须在真实 X（:0）跑**，会短暂占前台（每轮约 25s）
- **bench 脚本 display 变量是 `BENCH_DISPLAY` 不是 `DISPLAY`**：传错会把滚轮打进
  空 display，得到 webkit_cpu=0% 的假数据（本轮踩坑）。判伪手段：滚动中截屏 +
  `compare -metric AE` 像素 diff（webview 条带 42% 像素变化 = 滚动确实生效）
- :0 布局：主窗口 webview 条带在窗口相对 (400,300) 附近；flutter.dev hero 下半段
  是纯色，截图肉眼判断"没滚动"会误判，必须像素 diff 定量
- 进程级 CPU 归因：flutter 进程 = 插件+纹理路径成本；WebKitWebProcess = 页面渲染。
  跨 display 比较 flutter_cpu 无效（Xvfb 下引擎 llvmpipe 软渲染混入），只做同屏 A/B

### 遗留事项

- [ ] snapshot 回退路径动态节拍：scroll 实测 19fps 钉死 50ms 周期。若要提升回退
      路径帧率需缩短周期或 damage 驱动化（GPU 路径已解决，优先级降；Xvfb/无
      EGL pixmap import 环境仍靠回退路径）
- [ ] CI：Xvfb job 跑 snapshot 管线回归 + fps 断言（GPU 路径 Xvfb 不可测，见上）
- [ ] fps 打点目前为 debug 日志，若需发布态性能面板再考虑 telemetry 通道
- 此前遗留事项（IME 验证/InAppBrowser 运行时验证等）不变，见下文

## 2026-08-31 交付：修复 webview 右键 segfault（WebKitGTK 4.1 信号签名漂移）

### 根因（gdb 实证 + 上游源码核实）

右键即 SIGSEGV。gdb 断点链证明：`OnContextMenu` 的 `user_data` 不是 `InAppWebView*`，
而是 `WebKitHitTestResult*`（`g_type_name` 鉴定）。原因：**WebKitGTK 4.1 的
`context-menu` 信号自 2.40 起在 `context_menu` 与 `hit_test_result` 之间插入了
`GdkEvent*` 参数**（GIR/上游 WebKitWebViewGtk3.cpp 均核实），旧 4 参 C 回调按位
取参时 `hit_test_result` 落进 `user_data` 槽 → `self` 指向 HitTestResult 的内存
（GObject 头，refcount=2，与内存 dump 吻合）→ 读 `self->settings_`/unref
`self->pending_context_menu_`（垃圾指针）即崩。

版本矩阵（已核实）：

- webkit2gtk-4.1（2.40 首发 → 2.52+）：`context-menu` 与 `show-option-menu` 全版本
  带 `GdkEvent*`（4.1 系内稳定）
- WPE1/WPE2 2.40：旧 2 参签名；WPE1 2.52（main）：也加了 event（恒 NULL）
- `web-process-terminated` 的 reason 参数 2.40 起就有，非漂移，代码签名正确

### 修复（`flutter_inappwebview_linux/linux/in_app_webview/{in_app_webview.h,in_app_webview.cc}`）

1. `OnContextMenu`：`HAVE_WEBKIT_GTK` 分支改为 5 参
   `(web_view, context_menu, GdkEvent*, hit_test_result, user_data)`，event 显式
   `(void)`；WPE 分支保留旧 4 参（随 WPE 后端下线一并清理，头文件有注释）
2. `OnShowOptionMenu`：同源问题（`show-option-menu` 也带 `GdkEvent*`），
   GTK 分支同样补参——修复前点 `<select>` 下拉必崩，同类问题一次修完

### 验证（本机 X11，webkit2gtk-4.1 2.52.3）

- 修复前：`xdotool click 3` 100% 复现 SIGSEGV（栈顶
  `g_type_check_instance_is_fundamentally_a` ← `g_object_unref` ← OnContextMenu:4848）
- 修复后 `flutter build linux --debug` 通过；右键菜单正常弹出
  （Back/Forward/Stop/Reload/Inspect Element，disabled 项正确置灰）
- 连续两次右键（原 4848 unref 崩溃路径）零 SIGSEGV；点击 Reload 菜单项执行成功
  （Events 面板出现重载后事件），进程存活
- 复现脚本要点：`xdotool search --name "^flutter_inappwebview_example$"` 取真实
  窗口（勿用 class 名匹配，会命中 10x10 辅助窗口），webview 区域在窗口相对
  (400,300)

### 遗留事项

- [ ] WPE 模式若需支持 WPE1 2.52+：`OnContextMenu`/`OnShowOptionMenu` 需同样补
      `GdkEvent*`（恒 NULL）。当前 WPE 路径计划下个 minor 移除，未处理
- [ ] 其余已连接信号（load-changed/decide-policy/show-option-menu 等）已逐一对过
      2.52 GIR，签名均正确；后续若 WebKit 再加参数，可参照本次 GIR 对比法排查
- 此前遗留事项（IME 验证/性能 benchmark/InAppBrowser 运行时验证等）不变，见下文

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
