#include "webkit_gpu_capture.h"

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gdk/gdkx.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include "../utils/log.h"

namespace flutter_inappwebview_plugin {

namespace {

// === X 错误处理：作用域内静默专用连接的错误 ===
// XSetErrorHandler 为进程级；保存 GDK 的处理器，非本连接的错误原样转发。
thread_local XErrorHandler g_prev_handler = nullptr;
Display* g_scoped_display = nullptr;
int g_scoped_error_count = 0;

int ScopedXErrorHandler(Display* dpy, XErrorEvent* ev) {
  if (dpy != g_scoped_display) {
    if (g_prev_handler != nullptr) {
      return g_prev_handler(dpy, ev);
    }
    return 0;
  }
  g_scoped_error_count++;
  return 0;
}

// 作用域内（构造→析构间）屏蔽专用连接上的 X 错误；析构时 XSync 确保错误
// 同步送达后再恢复 GDK 处理器。仅主线程使用。
class ScopedXErrors {
 public:
  explicit ScopedXErrors(Display* dpy) : dpy_(dpy) {
    g_prev_handler = XSetErrorHandler(ScopedXErrorHandler);
    g_scoped_display = dpy;
    g_scoped_error_count = 0;
  }
  ~ScopedXErrors() {
    XSync(dpy_, False);
    XSetErrorHandler(g_prev_handler);
    g_prev_handler = nullptr;
    g_scoped_display = nullptr;
  }
  int count() const { return g_scoped_error_count; }

 private:
  Display* dpy_;
};

// === GLib 事件源：专用 X 连接的 damage 事件 ===
struct DamageSource {
  GSource base;
  GPollFD pfd{};
  Display* dpy = nullptr;
  int damage_event_base = 0;
  uint32_t last_w = 0, last_h = 0;         // 最后一条 damage 事件的 drawable 几何
  int32_t bbox_min_x = 0, bbox_min_y = 0;  // 本批 damage 区域包围盒（未累计时 max<min）
  int32_t bbox_max_x = -1, bbox_max_y = -1;
  // (drawable w, h, 本批包围盒是否覆盖全 drawable)
  std::function<void(uint32_t, uint32_t, bool)> on_damage;  // 主线程回调（已聚合）
};

gboolean DamageSourcePrepare(GSource* source, gint* timeout) {
  auto* s = reinterpret_cast<DamageSource*>(source);
  *timeout = -1;
  return XPending(s->dpy) > 0 ? TRUE : FALSE;
}

gboolean DamageSourceCheck(GSource* source) {
  auto* s = reinterpret_cast<DamageSource*>(source);
  return (s->pfd.revents & G_IO_IN) != 0 ? TRUE : FALSE;
}

gboolean DamageSourceDispatch(GSource* source, GSourceFunc, gpointer) {
  auto* s = reinterpret_cast<DamageSource*>(source);
  // 抽干全部 damage 事件再回调一次（天然聚合高刷动画的连续 damage）；
  // 记录最后一条事件的 drawable 几何：尺寸与该帧 backing 内容同源（服务端
  // 生成 damage 时的窗口真值），resize 过渡期不会拿旧尺寸配新 pixmap。
  // 同时累计本批 damage 区域包围盒：resize 后 WebKit 的全幅重绘以
  // 「包围盒覆盖全 drawable」为标志。
  XEvent ev;
  bool got_damage = false;
  while (XPending(s->dpy) > 0) {
    XNextEvent(s->dpy, &ev);
    if (ev.type == s->damage_event_base + XDamageNotify) {
      auto* dev = reinterpret_cast<XDamageNotifyEvent*>(&ev);
      const int32_t x2 = int32_t(dev->area.x) + dev->area.width;
      const int32_t y2 = int32_t(dev->area.y) + dev->area.height;
      if (s->bbox_max_x < s->bbox_min_x) {  // 本批第一条
        s->bbox_min_x = dev->area.x;
        s->bbox_min_y = dev->area.y;
      } else {
        s->bbox_min_x = std::min(s->bbox_min_x, int32_t(dev->area.x));
        s->bbox_min_y = std::min(s->bbox_min_y, int32_t(dev->area.y));
      }
      s->bbox_max_x = std::max(s->bbox_max_x, x2);
      s->bbox_max_y = std::max(s->bbox_max_y, y2);
      s->last_w = dev->geometry.width;
      s->last_h = dev->geometry.height;
      got_damage = true;
    }
  }
  if (got_damage && s->on_damage) {
    const bool covers_full = (s->bbox_max_x - s->bbox_min_x) >= int32_t(s->last_w) &&
                             (s->bbox_max_y - s->bbox_min_y) >= int32_t(s->last_h);
    // 复位包围盒，供下一批累计
    s->bbox_max_x = -1;
    s->bbox_max_y = -1;
    s->on_damage(s->last_w, s->last_h, covers_full);
  }
  return G_SOURCE_CONTINUE;
}

void DamageSourceFinalize(GSource* source) {
  auto* s = reinterpret_cast<DamageSource*>(source);
  s->on_damage = nullptr;
}

GSourceFuncs kDamageSourceFuncs = {DamageSourcePrepare,  DamageSourceCheck, DamageSourceDispatch,
                                   DamageSourceFinalize, nullptr,           nullptr};

}  // namespace

// === 私有实现 ===
struct WebKitGpuCapture::Impl {
  Display* dpy = nullptr;  // 专用 X 连接（仅主线程触碰）
  Window webview_xwin = 0;
  Damage damage = 0;
  int damage_event_base = 0;
  GSource* source = nullptr;  // 挂 GLib 默认主循环

  std::mutex mutex;           // 保护 pending/bound/free_queue（raster 线程参与）
  Pixmap pending_pixmap = 0;  // 最新帧别名（主线程产，raster 消费）
  uint32_t pending_w = 0;
  uint32_t pending_h = 0;
  Pixmap bound_pixmap = 0;  // 已绑定进纹理的别名（raster 线程写）
  EGLImageKHR bound_image = EGL_NO_IMAGE_KHR;
  uint32_t bound_w = 0;
  uint32_t bound_h = 0;
  std::vector<Pixmap> free_queue;  // 待主线程 XFreePixmap 的别名

  std::function<void()> on_frame_available;
  int64_t last_present_us = 0;             // present 节流（上限 ~125Hz）
  int64_t fps_window_start_us = 0;         // fps 打点窗口起点（0=未开始）
  uint32_t fps_frames = 0;                 // 窗口内 present 次数
  bool awaiting_full_repaint = false;      // resize 后等待 WebKit 全幅重绘
  uint32_t await_w = 0, await_h = 0;       // 等待中的目标几何
  int64_t await_start_us = 0;              // 几何变化时刻（超时兜底）
  int import_failures = 0;                 // 连续导入失败计数（用于一次性响亮日志）
  EGLDisplay engine_dpy = EGL_NO_DISPLAY;  // 首次导入时缓存的引擎显示句柄

  void DisposeDisplay() {
    if (dpy != nullptr) {
      XCloseDisplay(dpy);
      dpy = nullptr;
    }
  }
};

WebKitGpuCapture::WebKitGpuCapture() = default;

WebKitGpuCapture::~WebKitGpuCapture() {
  Stop();
}

bool WebKitGpuCapture::IsSupported(GtkWidget* webview_widget) {
  // 环境开关：FLUTTER_INAPPWEBVIEW_LINUX_GPU_CAPTURE=0 强制回退 snapshot 管线
  (void)webview_widget;  // 仅用 display 级检查；widget 级检查在 Start
  const gchar* env = g_getenv("FLUTTER_INAPPWEBVIEW_LINUX_GPU_CAPTURE");
  if (env != nullptr && g_strcmp0(env, "0") == 0) {
    debugLog("WebKitGpuCapture: disabled by FLUTTER_INAPPWEBVIEW_LINUX_GPU_CAPTURE=0");
    return false;
  }

  // X11 判定用默认 display（InitGtkHost 阶段 webview 尚未 realize，无 GdkWindow；
  // widget 级 XID 检查在 Start 时做，彼时已完成 show/realize）
  if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
    debugLog("WebKitGpuCapture: non-X11 backend, unsupported");
    return false;
  }

  Display* dpy = gdk_x11_get_default_xdisplay();
  int ev_base = 0, err_base = 0;
  if (!XCompositeQueryExtension(dpy, &ev_base, &err_base)) {
    debugLog("WebKitGpuCapture: XComposite extension missing");
    return false;
  }
  if (!XDamageQueryExtension(dpy, &ev_base, &err_base)) {
    debugLog("WebKitGpuCapture: XDamage extension missing");
    return false;
  }

  // EGL 能力：独立连接探测（与引擎同为 Mesa，代表性强）。
  // 引擎侧显示句柄要等首次 populate 才能拿到，此处只做定性行判断。
  Display* probe = XOpenDisplay(nullptr);
  if (probe == nullptr) {
    debugLog("WebKitGpuCapture: cannot open X probe connection");
    return false;
  }
  bool egl_ok = false;
  {
    EGLDisplay edpy = eglGetDisplay((EGLNativeDisplayType)probe);
    if (edpy != EGL_NO_DISPLAY) {
      EGLint maj = 0, min = 0;
      if (eglInitialize(edpy, &maj, &min)) {
        const char* exts = eglQueryString(edpy, EGL_EXTENSIONS);
        egl_ok = exts != nullptr && strstr(exts, "EGL_KHR_image_pixmap") != nullptr;
        eglTerminate(edpy);
      }
    }
  }
  XCloseDisplay(probe);
  if (!egl_ok) {
    debugLog("WebKitGpuCapture: EGL_KHR_image_pixmap unavailable");
    return false;
  }
  return true;
}

bool WebKitGpuCapture::Start(GtkWidget* webview_widget) {
  if (active_) {
    return true;
  }
  GdkWindow* gwk = gtk_widget_get_window(webview_widget);
  if (gwk == nullptr || !GDK_IS_X11_WINDOW(gwk)) {
    return false;
  }

  impl_ = new Impl();
  impl_->dpy = XOpenDisplay(nullptr);
  if (impl_->dpy == nullptr) {
    errorLog("WebKitGpuCapture: XOpenDisplay failed");
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  impl_->webview_xwin = gdk_x11_window_get_xid(gwk);

  int ev_base = 0, err_base = 0;
  if (!XDamageQueryExtension(impl_->dpy, &ev_base, &err_base)) {
    errorLog("WebKitGpuCapture: XDamage missing on dedicated connection");
    impl_->DisposeDisplay();
    delete impl_;
    impl_ = nullptr;
    return false;
  }
  impl_->damage_event_base = ev_base;

  {
    ScopedXErrors guard(impl_->dpy);
    XCompositeRedirectWindow(impl_->dpy, impl_->webview_xwin, CompositeRedirectManual);
    impl_->damage = XDamageCreate(impl_->dpy, impl_->webview_xwin, XDamageReportRawRectangles);
    if (guard.count() > 0) {
      errorLog("WebKitGpuCapture: redirect/damage setup failed (" + std::to_string(guard.count()) +
               " X errors)");
      impl_->DisposeDisplay();
      delete impl_;
      impl_ = nullptr;
      return false;
    }
  }

  // damage 事件源挂 GLib 主循环
  GSource* source = g_source_new(&kDamageSourceFuncs, sizeof(DamageSource));
  auto* ds = reinterpret_cast<DamageSource*>(source);
  ds->dpy = impl_->dpy;
  ds->damage_event_base = impl_->damage_event_base;
  ds->pfd.fd = XConnectionNumber(impl_->dpy);
  ds->pfd.events = G_IO_IN;
  g_source_add_poll(source, &ds->pfd);
  // on_damage 在 SetOnFrameAvailable 时接线
  g_source_attach(source, g_main_context_default());
  g_source_unref(source);
  impl_->source = source;

  active_ = true;
  debugLog("WebKitGpuCapture: started (window=0x" + std::to_string(impl_->webview_xwin) + ")");
  return true;
}

void WebKitGpuCapture::SetOnFrameAvailable(std::function<void()> callback) {
  if (impl_ == nullptr || !active_) {
    return;
  }
  auto* ds = reinterpret_cast<DamageSource*>(impl_->source);
  if (ds != nullptr) {
    ds->on_damage = [this](uint32_t w, uint32_t h, bool covers_full) {
      PresentOnce(w, h, covers_full);
    };
  }
  impl_->on_frame_available = std::move(callback);
  // 消费者接入立即补首帧
  PresentOnce();
}

void WebKitGpuCapture::Stop() {
  if (impl_ == nullptr) {
    active_ = false;
    return;
  }
  if (impl_->source != nullptr) {
    auto* ds = reinterpret_cast<DamageSource*>(impl_->source);
    ds->on_damage = nullptr;
    g_source_destroy(impl_->source);
    impl_->source = nullptr;
  }
  {
    ScopedXErrors guard(impl_->dpy);
    if (impl_->damage != 0) {
      XDamageDestroy(impl_->dpy, impl_->damage);
      impl_->damage = 0;
    }
    XCompositeUnredirectWindow(impl_->dpy, impl_->webview_xwin, CompositeRedirectManual);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->pending_pixmap != 0) {
      XFreePixmap(impl_->dpy, impl_->pending_pixmap);
      impl_->pending_pixmap = 0;
    }
    if (impl_->bound_pixmap != 0) {
      XFreePixmap(impl_->dpy, impl_->bound_pixmap);
      impl_->bound_pixmap = 0;
    }
    for (Pixmap px : impl_->free_queue) {
      XFreePixmap(impl_->dpy, px);
    }
    impl_->free_queue.clear();
    // bound_image 属引擎 EGLDisplay（首次导入时缓存句柄）：纹理先于本对象注销，
    // 引擎不再触碰；Mesa EGL 显示级调用线程安全，直接销毁。
    if (impl_->bound_image != EGL_NO_IMAGE_KHR && impl_->engine_dpy != EGL_NO_DISPLAY) {
      eglDestroyImageKHR(impl_->engine_dpy, impl_->bound_image);
      impl_->bound_image = EGL_NO_IMAGE_KHR;
    }
  }
  impl_->DisposeDisplay();
  delete impl_;
  impl_ = nullptr;
  active_ = false;
  debugLog("WebKitGpuCapture: stopped");
}

void WebKitGpuCapture::PresentOnce(uint32_t ev_w, uint32_t ev_h, bool ev_covers_full) {
  if (impl_ == nullptr || !active_ || !impl_->on_frame_available) {
    return;
  }
  // 节流：damage 风暴下限制 present 频率（~125Hz 上限）
  int64_t now = g_get_monotonic_time();
  if (now - impl_->last_present_us < 8000) {
    return;
  }
  impl_->last_present_us = now;

  // 回收 raster 线程用完的 pixmap 别名（X 操作仅限主线程）
  std::vector<Pixmap> to_free;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    to_free.swap(impl_->free_queue);
  }
  if (!to_free.empty()) {
    ScopedXErrors guard(impl_->dpy);
    for (Pixmap px : to_free) {
      XFreePixmap(impl_->dpy, px);
    }
  }

  // 尺寸来源：damage 事件自带的 drawable 几何（服务端真值，与该帧 backing
  // 内容生成时的窗口尺寸严格配对，稳态零往返）。ev 为 0 时（接线补首帧 /
  // resize 后强制补帧，无 damage 事件可查）退化为一次 XGetGeometry 取当前真值。
  // 注意不能用 GTK allocation 推断：GTK 端 allocation 更新与 X 服务端窗口
  // resize 分属两条连接、非原子，resize 过渡期会拿旧尺寸配新 pixmap（实测
  // 表现为拖拽窗口时画面拉伸/花屏）。
  uint32_t w = ev_w;
  uint32_t h = ev_h;
  if (w == 0 || h == 0) {
    ScopedXErrors guard(impl_->dpy);
    Window root = 0;
    int x = 0, y = 0;
    unsigned int bw = 0, depth = 0;
    unsigned int gw = 0, gh = 0;
    if (!XGetGeometry(impl_->dpy, impl_->webview_xwin, &root, &x, &y, &gw, &gh, &bw, &depth) ||
        gw == 0 || gh == 0 || guard.count() > 0) {
      return;
    }
    w = gw;
    h = gh;
  }
  Pixmap px;
  {
    ScopedXErrors guard(impl_->dpy);
    px = XCompositeNameWindowPixmap(impl_->dpy, impl_->webview_xwin);
    if (px == 0 || guard.count() > 0) {
      return;
    }
  }
  // resize 脏帧守门：几何与上次 present 不同 = backing 刚被服务端重分配，
  // 此时帧的内容只有旧尺寸区域有效，其余是未初始化显存（实测拖拽中出现
  // 脏块）。跳过几何变化后的首个 damage（服务端 resize 自带的全幅 damage），
  // 等 WebKit 对新尺寸的重绘（包围盒覆盖全 drawable）再恢复 present；等待
  // 期间引擎沿用旧帧（轻微拉伸，远好于脏块）。几何再变（拖拽连续步进）则
  // 刷新目标继续等。250ms 兜底：部分重绘场景不至永久卡旧帧。
  // 注意：等待期间 pending 仍为旧值，后续事件依旧满足「几何≠pending」，
  // 必须用 await_w/h 判定是否已在等同一目标尺寸，否则全幅重绘帧永远进不了
  // 放行分支（实测死锁：首帧永无落地，no frame yet 刷屏）。
  const bool resized_since_present = (impl_->pending_w != w || impl_->pending_h != h);
  const bool awaiting_this_size =
      impl_->awaiting_full_repaint && impl_->await_w == w && impl_->await_h == h;
  if (resized_since_present && !awaiting_this_size) {
    impl_->awaiting_full_repaint = true;
    impl_->await_w = w;
    impl_->await_h = h;
    impl_->await_start_us = now;
    return;
  }
  if (impl_->awaiting_full_repaint) {
    if (!ev_covers_full && now - impl_->await_start_us < 250000) {
      return;  // 未见全幅重绘且未超时，继续等
    }
    impl_->awaiting_full_repaint = false;
  }
  // fps 打点（debug）：每 5s 汇报实际出帧速率（过守门后的真实交付帧）
  impl_->fps_frames++;
  if (impl_->fps_window_start_us == 0) {
    impl_->fps_window_start_us = now;
  } else if (now - impl_->fps_window_start_us >= 5000000) {
    debugLog("WebKitGpuCapture: present fps=" +
             std::to_string(impl_->fps_frames * 1000000 / (now - impl_->fps_window_start_us)));
    impl_->fps_window_start_us = now;
    impl_->fps_frames = 0;
  }
  // 尺寸变化打点（debug）：resize 诊断（对照 snapshot 路径的尺寸日志）
  if (w != impl_->pending_w || h != impl_->pending_h) {
    debugLog("WebKitGpuCapture: present size " + std::to_string(w) + "x" + std::to_string(h));
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->pending_pixmap != 0) {
    impl_->free_queue.push_back(impl_->pending_pixmap);  // 未被消费，直接回收
  }
  impl_->pending_pixmap = px;
  impl_->pending_w = w;
  impl_->pending_h = h;
  impl_->on_frame_available();
}

bool WebKitGpuCapture::BindPendingFrameToTexture(uint32_t texture, uint32_t* out_width,
                                                 uint32_t* out_height, GError** error) {
  if (impl_ == nullptr || !active_) {
    g_set_error(error, g_quark_from_static_string("WebKitGpuCapture"), 1, "capture not active");
    return false;
  }

  // 无新帧但已有绑定内容：纹理保留原内容，报告既有尺寸
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->pending_pixmap == 0) {
      if (impl_->bound_pixmap != 0) {
        *out_width = impl_->bound_w;
        *out_height = impl_->bound_h;
        return true;
      }
      g_set_error(error, g_quark_from_static_string("WebKitGpuCapture"), 2, "no frame yet");
      return false;
    }
  }

  EGLDisplay edpy = eglGetCurrentDisplay();
  if (edpy == EGL_NO_DISPLAY) {
    g_set_error(error, g_quark_from_static_string("WebKitGpuCapture"), 3,
                "no current EGL display in populate");
    return false;
  }

  Pixmap take = 0;
  uint32_t w = 0, h = 0;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    take = impl_->pending_pixmap;
    w = impl_->pending_w;
    h = impl_->pending_h;
    impl_->pending_pixmap = 0;
  }

  const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
  EGLImageKHR img = eglCreateImageKHR(edpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
                                      (EGLClientBuffer)(uintptr_t)take, attrs);
  if (img == EGL_NO_IMAGE_KHR) {
    impl_->import_failures++;
    if (impl_->import_failures == 1 || impl_->import_failures % 120 == 0) {
      errorLog("WebKitGpuCapture: eglCreateImageKHR failed 0x" + std::to_string(eglGetError()) +
               " (" + std::to_string(impl_->import_failures) + " consecutive)");
    }
    // 归还 pending 以便下帧重试（槽位被占则直接回收）
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->pending_pixmap == 0) {
      impl_->pending_pixmap = take;
      impl_->pending_w = w;
      impl_->pending_h = h;
    } else {
      impl_->free_queue.push_back(take);
    }
    g_set_error(error, g_quark_from_static_string("WebKitGpuCapture"), 4,
                "eglCreateImageKHR failed");
    return false;
  }
  impl_->import_failures = 0;
  impl_->engine_dpy = edpy;

  // 导入成功：提交 bound 槽位（销毁旧 image，旧别名入回收队列）
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->bound_image != EGL_NO_IMAGE_KHR) {
      eglDestroyImageKHR(impl_->engine_dpy, impl_->bound_image);
    }
    if (impl_->bound_pixmap != 0) {
      impl_->free_queue.push_back(impl_->bound_pixmap);
    }
    impl_->bound_image = img;
    impl_->bound_pixmap = take;
    impl_->bound_w = w;
    impl_->bound_h = h;
  }

  // GL 绑定（raster 线程，引擎上下文 current）
  glBindTexture(GL_TEXTURE_2D, texture);
  epoxy_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  *out_width = w;
  *out_height = h;
  return true;
}

}  // namespace flutter_inappwebview_plugin
