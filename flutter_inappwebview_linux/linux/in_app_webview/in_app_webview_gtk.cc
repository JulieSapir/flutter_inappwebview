// in_app_webview_gtk.cc - WebKitGTK 后端专属实现
//
// 渲染管线（GPU 直通，唯一路径）:
//   WebKit 合成 → webview 原生 X 窗口（GPU）→ XComposite redirect pixmap
//   → EGLImage 导入 → FlTextureGL 纹理 → Flutter 采样。零 CPU 像素搬运。
//   宿主为 override-redirect 的 GTK_WINDOW_POPUP（屏外定位），帧驱动为
//   XDamage 事件（独立 X 连接）。实现在 webkit_gpu_capture.cc。
//   能力不满足时显式报错，不做回退（无 CPU 软渲染路径）。
//
// 输入桥：
//   Flutter 指针/滚轮/键盘事件 → 合成 GdkEvent（逻辑坐标）
//   → gtk_widget_event() 投递到 WebKitWebView 的 GdkWindow。
//   修饰键位序映射：Dart/WPE（C=1,S=2,A=4,M=8）→ GDK 掩码。
//
// 已知限制（显式声明，不做静默回退）：
//   - SendTouchEvent：GTK3 无法合法构造 GdkEventSequence，触摸注入暂不支持
//   - requestPointerLock/Unlock：WebKitGTK 无公开指针锁定 API，返回 false
//   - IME：popup 宿主下输入法行为待真实中文输入法环境回归验证（与离屏宿主机制相同）

#include <gdk/gdk.h>
#include <gdk/gdkx.h>  // GDK_IS_X11_WINDOW（GPU 直通宿主 X 窗口钉位）

#include <memory>

#include "../utils/log.h"
#include "in_app_webview.h"
#include "webkit_gpu_capture.h"

namespace flutter_inappwebview_plugin {

namespace {

// Dart/WPE 修饰键位序：Control=1, Shift=2, Alt=4, Meta=8
// GDK 掩码：SHIFT=1<<0, CONTROL=1<<2, MOD1(Alt)=1<<3, META=1<<28
inline guint DartModifiersToGdk(uint32_t mods) {
  guint out = 0;
  if (mods & 1u)
    out |= GDK_CONTROL_MASK;
  if (mods & 2u)
    out |= GDK_SHIFT_MASK;
  if (mods & 4u)
    out |= GDK_MOD1_MASK;
  if (mods & 8u)
    out |= GDK_META_MASK;
  return out;
}

// Flutter 按钮（1=primary, 2=secondary, 3=tertiary）→ GDK 按钮（1=左, 2=中, 3=右）
inline guint FlutterButtonToGdk(int button) {
  switch (button) {
    case 2:
      return 3;  // secondary -> Right
    case 3:
      return 2;  // tertiary -> Middle
    case 1:
    default:
      return 1;  // primary -> Left
  }
}

// 合成事件必须携带真实 GdkDevice：gdk_event_new 创建的事件 device 字段为 NULL，
// WebKit/GTK 内部处理事件（crossing/focus 路径）会调用 gdk_device_get_source
// 等接口，NULL 触发 "assertion 'GDK_IS_DEVICE (device)' failed"（焦点移动时报错）。
// gdk_event_set_device 内部负责 g_object_ref，与 gdk_event_free 的 unref 对称。
GdkDevice* GtkGetSeatDevice(bool keyboard) {
  GdkDisplay* display = gdk_display_get_default();
  if (display == nullptr) {
    return nullptr;
  }
  GdkSeat* seat = gdk_display_get_default_seat(display);
  if (seat == nullptr) {
    return nullptr;
  }
  return keyboard ? gdk_seat_get_keyboard(seat) : gdk_seat_get_pointer(seat);
}

void GtkSetEventDevice(GdkEvent* event, bool keyboard) {
  GdkDevice* device = GtkGetSeatDevice(keyboard);
  if (device != nullptr) {
    gdk_event_set_device(event, device);
  }
}

}  // namespace

// === 离屏宿主生命周期 ===

void InAppWebView::InitGtkHost() {
  if (webview_ == nullptr) {
    return;
  }

  // GPU 直通能力检查（X11 + Composite/Damage + EGL_KHR_image_pixmap）。
  // GPU 直通是唯一渲染管线：能力不满足时宿主照常创建（widget 正常挂载），
  // 但无纹理输出，显式报错，不做回退。
  const bool gpu_capable = WebKitGpuCapture::IsSupported(GTK_WIDGET(webview_));
  if (!gpu_capable) {
    errorLog(
        "InAppWebView(gtk): GPU direct capture unsupported (requires native X11 + "
        "Composite/Damage + EGL_KHR_image_pixmap); no rendering output will be available");
  }

  // override-redirect popup 定位到完全屏外：XComposite 捕获需要真实原生
  // X 窗口，GTK3 离屏宿主不产生 X 窗口（探针实证：窗口树中无 webview 窗口）。
  // 坐标取 -(2*尺寸+256)，任意窗口尺寸均落在 X11 16-bit 坐标界内且不可见。
  gtk_host_window_ = GTK_WINDOW(gtk_window_new(GTK_WINDOW_POPUP));
  gtk_window_move(gtk_host_window_, -(2 * width_ + 256), -(2 * height_ + 256));
  gtk_container_add(GTK_CONTAINER(gtk_host_window_), GTK_WIDGET(webview_));

  // 保证可聚焦（grab_focus 前置条件）
  gtk_widget_set_can_focus(GTK_WIDGET(webview_), TRUE);

  // 焦点取证：WebKit 的 caret 绘制依赖 focus-in/out 到达 WebKitWebView
  // （ViewIsFocused activity state）。离屏 popup 宿主不持有 X toplevel focus，
  // GTK 内部 focus widget 状态是唯一驱动力，打点观测链路是否接通。
  g_signal_connect(webview_, "focus-in-event",
                   G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer) {
                     debugLog("InAppWebView(gtk): WebKit focus-in-event received");
                     return FALSE;
                   }),
                   nullptr);
  g_signal_connect(webview_, "focus-out-event",
                   G_CALLBACK(+[](GtkWidget*, GdkEventFocus*, gpointer) {
                     debugLog("InAppWebView(gtk): WebKit focus-out-event received");
                     return FALSE;
                   }),
                   nullptr);

  // 尺寸分配：WebKit layout viewport 必须与 Flutter 纹理逻辑尺寸一致。
  // 关键点（RCA 实证）：宿主从 map 到屏幕前 GTK 布局轮不会自动跑，必须直接
  // size_allocate 宿主窗口本身，让 GtkBin 的正常分配链路把尺寸传导给 webview。
  gtk_window_resize(gtk_host_window_, width_, height_);
  {
    GtkAllocation win_alloc = {0, 0, width_, height_};
    gtk_widget_size_allocate(GTK_WIDGET(gtk_host_window_), &win_alloc);
  }

  gtk_widget_show(GTK_WIDGET(webview_));
  gtk_widget_show(GTK_WIDGET(gtk_host_window_));

  // map 后立即用 gdk 路径把宿主 X 窗口钉到位。RCA（初次载入脏）：
  // gtk_window_resize 的请求要等 GTK 主循环布局轮才落到 X 服务端（实测 ~1.2s），
  // 期间 X 窗口停在 GTK 默认 800x600——Start 捕获的几何错误，且该阶段 WebKit
  // 未绘制，backing 全是未初始化显存。popup 宿主完全走 gdk 路径（同 setSize
  // 的单写入者原则）：gdk_window_move_resize 直接发 XConfigureWindow，服务端
  // 立即生效，顺带重钉屏外定位。
  GdkWindow* host_gdk = gtk_widget_get_window(GTK_WIDGET(gtk_host_window_));
  if (host_gdk != nullptr && GDK_IS_X11_WINDOW(host_gdk)) {
    gdk_window_move_resize(host_gdk, -(2 * width_ + 256), -(2 * height_ + 256), width_, height_);
  }

  // 宿主就绪后启动捕获（redirect + damage 源 + 首帧别名）。
  // 帧输出回调在纹理注册后由 AttachGpuCaptureOutput 接线；此前 damage 只计数。
  gpu_capture_ = std::make_unique<WebKitGpuCapture>();
  if (!gpu_capture_->Start(GTK_WIDGET(webview_))) {
    // 启动失败（redirect 被拒等）：显式报错，无回退管线。
    errorLog("InAppWebView(gtk): GPU capture start failed; no rendering output");
    gpu_capture_.reset();
  } else {
    debugLog("InAppWebView(gtk): GPU direct capture active");
  }

  // 监听 widget 自身 scale 变化（HiDPI 下重出帧）
  gtk_scale_handler_id_ = g_signal_connect(
      webview_, "notify::scale-factor", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer user_data) {
        auto* self = static_cast<InAppWebView*>(user_data);
        int new_scale = gtk_widget_get_scale_factor(GTK_WIDGET(self->webview()));
        if (new_scale > 0 && static_cast<double>(new_scale) != self->scale_factor_) {
          self->scale_factor_ = static_cast<double>(new_scale);
        }
        self->RequestSnapshot();
      }),
      this);

  if (gtk_window_ != nullptr) {
    // 顶层窗口 scale 变化（显示器切换/系统缩放设置）同步到快照
    gtk_window_scale_handler_id_ =
        g_signal_connect(gtk_window_, "notify::scale-factor",
                         G_CALLBACK(+[](GObject* object, GParamSpec*, gpointer user_data) {
                           auto* self = static_cast<InAppWebView*>(user_data);
                           int new_scale = gtk_widget_get_scale_factor(GTK_WIDGET(object));
                           if (new_scale > 0) {
                             self->scale_factor_ = static_cast<double>(new_scale);
                           }
                           self->RequestSnapshot();
                         }),
                         this);
  }

  RequestSnapshot();
}

void InAppWebView::ShutdownGtkHost() {
  // 先停捕获：XDamage/redirect 依赖宿主 X 窗口存活
  if (gpu_capture_ != nullptr) {
    gpu_capture_->Stop();
    gpu_capture_.reset();
  }
  if (gtk_scale_handler_id_ != 0 && webview_ != nullptr) {
    g_signal_handler_disconnect(webview_, gtk_scale_handler_id_);
    gtk_scale_handler_id_ = 0;
  }
  if (gtk_window_scale_handler_id_ != 0 && gtk_window_ != nullptr) {
    g_signal_handler_disconnect(gtk_window_, gtk_window_scale_handler_id_);
    gtk_window_scale_handler_id_ = 0;
  }
  if (gtk_host_window_ != nullptr) {
    // 窗口销毁会连带销毁 webview widget（容器持有），webview_ 的 GObject
    // 引用随后在析构函数中 unref 释放
    gtk_widget_destroy(GTK_WIDGET(gtk_host_window_));
    gtk_host_window_ = nullptr;
  }
}

// === GPU 直通帧输出 ===

bool InAppWebView::IsGpuCaptureActive() const {
  return gpu_capture_ != nullptr && gpu_capture_->IsActive();
}

WebKitGpuCapture* InAppWebView::gpu_capture() const {
  return gpu_capture_.get();
}

void InAppWebView::AttachGpuCaptureOutput() {
  if (gpu_capture_ == nullptr || !gpu_capture_->IsActive()) {
    return;
  }
  // damage → on_frame_available_（CustomPlatformView 已将其接到
  // fl_texture_registrar_mark_texture_frame_available）。接线时补首帧。
  gpu_capture_->SetOnFrameAvailable([this]() {
    if (on_frame_available_) {
      on_frame_available_();
    }
  });
}

void InAppWebView::RequestSnapshot() {
  if (webview_ == nullptr || is_disposing_.load()) {
    return;
  }
  // GPU 直通强制补帧：重取当前内容别名并入队（帧驱动本体为 XDamage，
  // 由 resize/scale 变化处调用）。方法名保留以复用历史调用点。
  if (gpu_capture_ != nullptr && gpu_capture_->IsActive()) {
    gpu_capture_->PresentOnce();
  }
}

// === 输入桥：GdkEvent 合成与投递 ===

void InAppWebView::DispatchGdkEvent(GdkEvent* event) {
  if (event == nullptr || webview_ == nullptr) {
    if (event != nullptr) {
      gdk_event_free(event);
    }
    return;
  }
  GtkWidget* widget = GTK_WIDGET(webview_);
  GdkWindow* window = gtk_widget_get_window(widget);
  if (window == nullptr) {
    // 未 realize（例如宿主尚未挂载）：显式丢弃并记录，不做静默吞掉
    debugLog("InAppWebView(gtk): drop synthesized event, widget window not realized");
    gdk_event_free(event);
    return;
  }
  // 事件必须指向 widget 的 GdkWindow 才能被 GTK 事件系统正确路由。
  // GDK3 无 gdk_event_set_window，直接赋值并 ref（gdk_event_free 内部 unref）。
  g_object_ref(window);
  event->any.window = window;
  gtk_widget_event(widget, event);
  gdk_event_free(event);  // GTK3：事件由调用方释放
}

// === 输入合成实现 ===

void InAppWebView::GtkSetCursorPos(double x, double y) {
  if (webview_ == nullptr) {
    return;
  }

  GdkEventMotion* ev = reinterpret_cast<GdkEventMotion*>(gdk_event_new(GDK_MOTION_NOTIFY));
  ev->send_event = FALSE;
  ev->time = static_cast<guint32>(g_get_monotonic_time() / 1000);
  ev->x = x;
  ev->y = y;
  ev->x_root = x;
  ev->y_root = y;
  // state：键盘修饰（WPE 位序转 GDK）+ 已按下按钮（GDK 按钮掩码位序 1<<8+，
  // 与 WPEPlatform 分支的 button_state_ 位序一致，拖拽/文本选择依赖它）
  ev->state = DartModifiersToGdk(current_modifiers_) | button_state_;
  GtkSetEventDevice(reinterpret_cast<GdkEvent*>(ev), false);
  DispatchGdkEvent(reinterpret_cast<GdkEvent*>(ev));
}

void InAppWebView::GtkSetPointerButton(int kind, int button, int clickCount) {
  if (webview_ == nullptr) {
    return;
  }

  const guint gdk_button = FlutterButtonToGdk(button);
  // GDK 按钮掩码位序：BUTTON1=1<<8, BUTTON2=1<<9, BUTTON3=1<<10
  const uint32_t button_modifier_bit = 1u << (7 + gdk_button);

  GdkEventType event_type;
  switch (static_cast<WpePointerEventKind>(kind)) {
    case WpePointerEventKind::Down:
      event_type = GDK_BUTTON_PRESS;
      break;
    case WpePointerEventKind::Up:
      event_type = GDK_BUTTON_RELEASE;
      break;
    default:
      // Ignore enter/leave/cancel etc for button events
      return;
  }

  // Down + clickCount >= 2 映射 GDK 双击/三击语义
  GdkEventType actual_type = event_type;
  if (event_type == GDK_BUTTON_PRESS) {
    if (clickCount == 2) {
      actual_type = GDK_2BUTTON_PRESS;
    } else if (clickCount >= 3) {
      actual_type = GDK_3BUTTON_PRESS;
    }
  }

  const guint32 time = static_cast<guint32>(g_get_monotonic_time() / 1000);

  if (event_type == GDK_BUTTON_PRESS) {
    button_state_ |= button_modifier_bit;
  }

  // 先发 motion 确保 WebKit 光标位置正确（与 WPE 分支一致）
  GdkEventMotion* motion = reinterpret_cast<GdkEventMotion*>(gdk_event_new(GDK_MOTION_NOTIFY));
  motion->send_event = FALSE;
  motion->time = time;
  motion->x = cursor_x_;
  motion->y = cursor_y_;
  motion->x_root = cursor_x_;
  motion->y_root = cursor_y_;
  motion->state = DartModifiersToGdk(current_modifiers_) | button_state_;
  GtkSetEventDevice(reinterpret_cast<GdkEvent*>(motion), false);
  DispatchGdkEvent(reinterpret_cast<GdkEvent*>(motion));

  GdkEventButton* btn = reinterpret_cast<GdkEventButton*>(gdk_event_new(actual_type));
  btn->send_event = FALSE;
  btn->time = time;
  btn->x = cursor_x_;
  btn->y = cursor_y_;
  btn->x_root = cursor_x_;
  btn->y_root = cursor_y_;
  btn->button = gdk_button;
  btn->state = DartModifiersToGdk(current_modifiers_) | button_state_;
  GtkSetEventDevice(reinterpret_cast<GdkEvent*>(btn), false);
  DispatchGdkEvent(reinterpret_cast<GdkEvent*>(btn));

  if (event_type == GDK_BUTTON_RELEASE) {
    button_state_ &= ~button_modifier_bit;
  }
}

void InAppWebView::GtkSetScrollDelta(double dx, double dy) {
  if (webview_ == nullptr) {
    return;
  }

  // 单位还原（RCA：页面滑动过快）：
  //
  // 1) Flutter engine（fl_scrolling_manager.cc）把 GDK scroll delta 乘
  //    kScrollOffsetMultiplier(53) * scale_factor 后发给 Dart（物理像素单位）；
  //    GDK_SCROLL_SMOOTH 事件的 delta 是 GDK 原生单位，WebKit 按内部步长换算，
  //    像素值原样回填会被再次换算（旧实现滚轮一格滚 ~2120px，即 53*40 双重放大）。
  //
  // 2) WebKitGTK SMOOTH delta→px 步长（原生标尺页实测标定，2026-09-04）：
  //      视口 204 高：delta 1.0 → 34px（= 204/6）
  //      视口 320 高：delta 1.0 → 46px（≈ 320/7，充分收敛后复测一致）
  //    拟合 pxPerUnit = max(34, H/7)（H = 逻辑视口高）。两点定线存在外推
  //    不确定性（±10% 量级），远优于修复前的 53 倍放大；后续如需精化可再标定。
  //
  // 目标语义：与 Flutter 桌面其他应用一致——滚轮一格 53 逻辑像素、触控板
  // panDelta 1px:1px（Dart 侧 panDelta 与 scrollDelta 同像素语义，统一换算）。
  const double scale = scale_factor_ > 0 ? scale_factor_ : 1.0;
  const double viewport_h = height_ > 0 ? static_cast<double>(height_) : 204.0;
  const double px_per_unit = viewport_h > 238.0 ? viewport_h / 7.0 : 34.0;
  const double gdk_delta_per_px = 1.0 / (scale * px_per_unit);

  GdkEventScroll* ev = reinterpret_cast<GdkEventScroll*>(gdk_event_new(GDK_SCROLL));
  ev->send_event = FALSE;
  ev->time = static_cast<guint32>(g_get_monotonic_time() / 1000);
  ev->x = cursor_x_;
  ev->y = cursor_y_;
  ev->direction = GDK_SCROLL_SMOOTH;
  ev->delta_x = dx * gdk_delta_per_px;
  ev->delta_y = dy * gdk_delta_per_px;
  ev->state = DartModifiersToGdk(current_modifiers_);
  GtkSetEventDevice(reinterpret_cast<GdkEvent*>(ev), false);
  DispatchGdkEvent(reinterpret_cast<GdkEvent*>(ev));
}

void InAppWebView::GtkSetFocused(bool focused) {
  if (webview_ == nullptr) {
    return;
  }

  // 焦点事件补链路（RCA：输入框 caret 不显示）：离屏 popup 宿主永不持有
  // X toplevel focus，gtk_widget_grab_focus 只更新 GTK 内部 focus widget
  // 状态，不会向 WebKitWebView 投递 GDK_FOCUS_CHANGE（focus-in/out 事件
  // 实测零到达），WebKit 的 ViewIsFocused activity state 拉不起来 → caret
  // 不绘制。键盘事件不走 focus 通道所以输入仍有效，形成“能输入但无光
  // 标”。显式合成 focus 事件（与其他合成输入同模式）；WebKitGTK 内部
  // isFocused 状态有幂等保护，重复投递无害。
  GdkEventFocus* ev = reinterpret_cast<GdkEventFocus*>(gdk_event_new(GDK_FOCUS_CHANGE));
  ev->send_event = FALSE;
  ev->in = focused ? TRUE : FALSE;
  GtkSetEventDevice(reinterpret_cast<GdkEvent*>(ev), true);
  DispatchGdkEvent(reinterpret_cast<GdkEvent*>(ev));
}

void InAppWebView::GtkSendKeyEvent(int type, int64_t keyCode, int scanCode, uint32_t modifiers) {
  if (webview_ == nullptr) {
    return;
  }

  // type: 0=down, 1=up, 2=repeat（repeat 视为 down）
  GdkEventType event_type;
  switch (type) {
    case 0:
    case 2:
      event_type = GDK_KEY_PRESS;
      break;
    case 1:
      event_type = GDK_KEY_RELEASE;
      break;
    default:
      return;
  }

  GdkEventKey* ev = reinterpret_cast<GdkEventKey*>(gdk_event_new(event_type));
  ev->send_event = FALSE;
  ev->time = static_cast<guint32>(g_get_monotonic_time() / 1000);
  // Dart 传 XKB keysym（与 GDK keyval 同源）与 X keycode
  ev->keyval = static_cast<guint>(keyCode);
  ev->hardware_keycode = static_cast<guint16>(scanCode);
  ev->state = DartModifiersToGdk(modifiers);
  ev->string = nullptr;
  ev->length = 0;
  GtkSetEventDevice(reinterpret_cast<GdkEvent*>(ev), true);
  DispatchGdkEvent(reinterpret_cast<GdkEvent*>(ev));
}

void InAppWebView::GtkSendTouchEvent(
    int type, int id, double x, double y,
    const std::vector<std::tuple<int, double, double, int>>& touchPoints) {
  // GTK3 无法合法构造 GdkEventSequence（不透明类型），且 WebKitGTK 触摸处理
  // 依赖真实触控设备序列。桌面场景以鼠标为主；触摸注入显式报错而非静默丢弃。
  (void)type;
  (void)id;
  (void)x;
  (void)y;
  (void)touchPoints;
  errorLog(
      "InAppWebView(gtk): SendTouchEvent is not supported on the WebKitGTK "
      "backend (GdkEventSequence cannot be synthesized)");
}

}  // namespace flutter_inappwebview_plugin
