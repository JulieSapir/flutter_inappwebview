// in_app_webview_gtk.cc - WebKitGTK 后端（backend=gtk，默认）专属实现
//
// 渲染管线：
//   webkit_web_view_snapshot()（异步，GTK 主线程回调）
//   → cairo ARGB32 surface（小端机内存布局为 BGRA）
//   → ConvertARGB32ToRGBA()（SIMD BGRA→RGBA，复用 simd_convert.h）
//   → 共享三缓冲 pixel_buffers_（与 WPE SHM 路径同一消费端）
//   → FlPixelBufferTexture（纹理类零改动）
//
// 帧驱动：
//   RegisterEventHandlers() 已通过 webkit_web_view_add_frame_displayed_callback
//   连接 OnFrameDisplayed，WebKit 每完成一帧渲染即回调 → RequestSnapshot()。
//   snapshot_pending_ 防重入：上一帧未回则仅置 dirty 标记，回调后补拍，
//   避免快照请求在 GTK 主循环排队堆积。
//
// 输入桥：
//   Flutter 指针/滚轮/键盘事件 → 合成 GdkEvent（逻辑坐标，与 WPEPlatform 分支一致）
//   → gtk_widget_event() 投递到 WebKitWebView 的 GdkWindow。
//   修饰键位序映射：Dart/WPE（C=1,S=2,A=4,M=8）→ GDK 掩码。
//
// 离屏宿主：
//   GtkOffscreenWindow 提供不映射到屏幕的 GdkWindow 体系，
//   使 WebKitWebView 可 realize、渲染、接收合成事件。
//
// 已知限制（显式声明，不做静默回退）：
//   - SendTouchEvent：GTK3 无法合法构造 GdkEventSequence，触摸注入暂不支持
//   - requestPointerLock/Unlock：WebKitGTK 无公开指针锁定 API，返回 false

#include "in_app_webview.h"

#ifdef HAVE_WEBKIT_GTK

#include <gdk/gdk.h>

#include <algorithm>
#include <cstring>

#include "../utils/log.h"
#include "simd_convert.h"

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

  // GtkOffscreenWindow：GTK3 官方离屏宿主容器。
  // 挂入的 widget 获得真实 GdkWindow（GDK_WINDOW_OFFSCREEN），可 realize、
  // 渲染、接收合成事件，但不映射到屏幕。官方示例用法为 show_all()。
  gtk_host_window_ = GTK_WINDOW(gtk_offscreen_window_new());
  gtk_container_add(GTK_CONTAINER(gtk_host_window_), GTK_WIDGET(webview_));

  // 保证可聚焦（grab_focus 前置条件）
  gtk_widget_set_can_focus(GTK_WIDGET(webview_), TRUE);

  // 尺寸分配：WebKit layout viewport 必须与 Flutter 纹理逻辑尺寸一致。
  // 关键点（RCA 实证）：GtkOffscreenWindow 从不 map 到屏幕，gtk_window_resize
  // 设置的 default size 永远等不到 configure 事件生效，宿主 allocation 停在 1x1；
  // GTK 主循环每轮布局会用宿主 allocation 覆盖子 widget，导致 snapshot 出图在
  // 1x1 与真实尺寸间摇摆（页面只剩首帧残影）。因此必须直接 size_allocate
  // 宿主窗口本身，让 GtkBin 的正常分配链路把尺寸传导给 webview。
  gtk_window_resize(gtk_host_window_, width_, height_);
  {
    GtkAllocation win_alloc = {0, 0, width_, height_};
    gtk_widget_size_allocate(GTK_WIDGET(gtk_host_window_), &win_alloc);
  }

  gtk_widget_show(GTK_WIDGET(webview_));
  gtk_widget_show(GTK_WIDGET(gtk_host_window_));

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
  StopSnapshotTicker();
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
  snapshot_pending_ = false;
  snapshot_dirty_ = false;
}

// === Snapshot 管线 ===

void InAppWebView::RequestSnapshot() {
  if (webview_ == nullptr || is_disposing_.load()) {
    return;
  }
  if (snapshot_pending_) {
    // 上一帧仍在途：仅标记脏，OnSnapshotReady 完成后补拍
    snapshot_dirty_ = true;
    return;
  }
  snapshot_pending_ = true;

  const int w = std::max(1, static_cast<int>(width_ * scale_factor_));
  const int h = std::max(1, static_cast<int>(height_ * scale_factor_));
  (void)w;
  (void)h;

  // GTK 老 API：无宽高参数，VISIBLE region 按当前 viewport allocation 出图
  // （allocation 由 InitGtkHost/setSize 的 gtk_window_resize 同步）。
  // WPE 2.40+ 的 snapshot() 带宽高参数（见 webkit_include.h）。
  webkit_web_view_get_snapshot(webview_, WEBKIT_SNAPSHOT_REGION_VISIBLE,
                               WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr, OnSnapshotReady, this);
}

void InAppWebView::OnSnapshotReady(GObject* source_object, GAsyncResult* result,
                                   gpointer user_data) {
  auto* self = static_cast<InAppWebView*>(user_data);

  GError* error = nullptr;
  cairo_surface_t* surface =
      webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source_object), result, &error);
  if (error != nullptr) {
    // 启动早期（页面未加载/未出帧）失败属正常，tick 会重试；降级为 debug 日志
    debugLog(std::string("InAppWebView(gtk): snapshot failed: ") +
             (error->message != nullptr ? error->message : "unknown"));
    g_error_free(error);
    self->snapshot_pending_ = false;
    return;
  }

  self->DeliverSnapshot(surface);

  if (surface != nullptr) {
    cairo_surface_destroy(surface);
  }

  self->snapshot_pending_ = false;
  // 期间有新请求：补拍一帧
  if (self->snapshot_dirty_ && !self->is_disposing_.load()) {
    self->snapshot_dirty_ = false;
    self->RequestSnapshot();
  }
}

void InAppWebView::DeliverSnapshot(cairo_surface_t* surface) {
  if (is_disposing_.load() || surface == nullptr) {
    return;
  }
  if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE) {
    errorLog("InAppWebView(gtk): unexpected snapshot surface type");
    return;
  }

  const uint32_t w = static_cast<uint32_t>(cairo_image_surface_get_width(surface));
  const uint32_t h = static_cast<uint32_t>(cairo_image_surface_get_height(surface));
  if (w == 0 || h == 0) {
    return;
  }

  // [debug] 出图尺寸 vs 两级 allocation vs 逻辑尺寸，验证 GtkBin 传导链路
  {
    static uint32_t last_w = 0, last_h = 0;
    if (w != last_w || h != last_h) {
      last_w = w;
      last_h = h;
      GtkAllocation alloc = {0, 0, 0, 0};
      gtk_widget_get_allocation(GTK_WIDGET(webview_), &alloc);
      GtkAllocation win_alloc = {0, 0, 0, 0};
      if (gtk_host_window_ != nullptr) {
        gtk_widget_get_allocation(GTK_WIDGET(gtk_host_window_), &win_alloc);
      }
      debugLog("InAppWebView(gtk): snapshot " + std::to_string(w) + "x" + std::to_string(h) +
               " | win alloc " + std::to_string(win_alloc.width) + "x" +
               std::to_string(win_alloc.height) + " | webview alloc " +
               std::to_string(alloc.width) + "x" + std::to_string(alloc.height) + " | logical " +
               std::to_string(static_cast<int>(width_)) + "x" +
               std::to_string(static_cast<int>(height_)));
    }
  }

  const int stride = cairo_image_surface_get_stride(surface);
  const uint8_t* src = cairo_image_surface_get_data(surface);
  if (src == nullptr) {
    return;
  }

  // 写入三缓冲（与 WPE SHM 路径同一消费端：CopyPixelBufferTo）
  size_t write_idx = write_buffer_index_.load(std::memory_order_relaxed);
  auto& pixel_buffer = pixel_buffers_[write_idx];
  const size_t needed = static_cast<size_t>(w) * h * 4;
  if (pixel_buffer.data.size() != needed) {
    pixel_buffer.data.resize(needed);
  }

  uint8_t* dst = pixel_buffer.data.data();
  // cairo ARGB32（小端为 BGRA）→ RGBA；stride 非 4 对齐时逐行转换
  if (stride == static_cast<int>(w * 4)) {
    ConvertARGB32ToRGBA(src, dst, static_cast<int>(w), static_cast<int>(h), stride);
  } else {
    for (uint32_t row = 0; row < h; ++row) {
      ConvertARGB32ToRGBA(src + static_cast<size_t>(row) * stride,
                          dst + static_cast<size_t>(row) * w * 4, static_cast<int>(w), 1, stride);
    }
  }

  pixel_buffer.width = w;
  pixel_buffer.height = h;

  {
    std::lock_guard<std::mutex> swap_lock(buffer_swap_mutex_);
    read_buffer_index_.store(write_idx, std::memory_order_release);
    write_buffer_index_.store((write_idx + 1) % kNumBuffers, std::memory_order_relaxed);
  }

  // 帧数据就绪后通知 Flutter（纹理 MarkTextureFrameAvailable）
  if (on_frame_available_) {
    on_frame_available_();
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

// === 帧驱动节拍器 ===
// WebKitGTK 无 per-frame 回调（webkit_web_view_add_frame_displayed_callback 为
// WPE 专属）。50ms 节拍 + snapshot_pending_ 防重入：
// 快照耗时超过节拍时自动降速，避免 GTK 主循环排队堆积。
// 调优点：接入 webkit_web_view_is_loading 动态周期（见 Phase 4 benchmark）。

void InAppWebView::StartSnapshotTicker() {
  if (snapshot_ticker_source_ != 0) {
    return;
  }
  snapshot_ticker_source_ = g_timeout_add(50, OnSnapshotTick, this);
}

void InAppWebView::StopSnapshotTicker() {
  if (snapshot_ticker_source_ != 0) {
    g_source_remove(snapshot_ticker_source_);
    snapshot_ticker_source_ = 0;
  }
}

gboolean InAppWebView::OnSnapshotTick(gpointer user_data) {
  auto* self = static_cast<InAppWebView*>(user_data);
  self->RequestSnapshot();
  return G_SOURCE_CONTINUE;
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

  GdkEventScroll* ev = reinterpret_cast<GdkEventScroll*>(gdk_event_new(GDK_SCROLL));
  ev->send_event = FALSE;
  ev->time = static_cast<guint32>(g_get_monotonic_time() / 1000);
  ev->x = cursor_x_;
  ev->y = cursor_y_;
  ev->direction = GDK_SCROLL_SMOOTH;
  ev->delta_x = dx;
  ev->delta_y = dy;
  ev->state = DartModifiersToGdk(current_modifiers_);
  GtkSetEventDevice(reinterpret_cast<GdkEvent*>(ev), false);
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

#endif  // HAVE_WEBKIT_GTK
