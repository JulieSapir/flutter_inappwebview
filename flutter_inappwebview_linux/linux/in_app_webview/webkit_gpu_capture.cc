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
  // (drawable w, h, 本批 bbox) —— bbox 供守门逻辑跨批累计并集
  std::function<void(uint32_t, uint32_t, bool, int32_t, int32_t, int32_t, int32_t)> on_damage;
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
    s->on_damage(s->last_w, s->last_h, covers_full, s->bbox_min_x, s->bbox_min_y, s->bbox_max_x,
                 s->bbox_max_y);
    // 复位包围盒，供下一批累计
    s->bbox_max_x = -1;
    s->bbox_max_y = -1;
  }
  return G_SOURCE_CONTINUE;
}

void DamageSourceFinalize(GSource* source) {
  auto* s = reinterpret_cast<DamageSource*>(source);
  s->on_damage = nullptr;
}

GSourceFuncs kDamageSourceFuncs = {DamageSourcePrepare,  DamageSourceCheck, DamageSourceDispatch,
                                   DamageSourceFinalize, nullptr,           nullptr};

// 1x1 白底占位纹理（引擎 raster 线程、上下文 current 时调用）。
// 首帧未到期间 populate 若直接失败，flutter_linux 引擎会对每次合成 g_warning
// （"no frame yet"）——输入框聚焦时光标闪烁每帧合成一次即逐帧刷屏。改为交付
// 一张白底占位纹理：与 webkit_web_view_set_background_color 的白底同源，
// 观感等同「页面加载中」，真帧到达后由 EGLImage 绑定整体替换存储。
bool EnsurePlaceholderTexture(uint32_t texture, GError** error) {
  static const GLubyte kWhite[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kWhite);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  if (glGetError() != GL_NO_ERROR) {
    g_set_error(error, g_quark_from_static_string("WebKitGpuCapture"), 5,
                "placeholder texture allocation failed");
    return false;
  }
  return true;
}

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
  int64_t last_present_us = 0;                         // present 节流（上限 ~125Hz）
  int64_t fps_window_start_us = 0;                     // fps 打点窗口起点（0=未开始）
  uint32_t fps_frames = 0;                             // 窗口内 present 次数
  bool awaiting_full_repaint = false;                  // resize/首帧后等待 WebKit 全幅重绘
  uint32_t await_w = 0, await_h = 0;                   // 等待中的目标几何
  int64_t await_start_us = 0;                          // 几何变化时刻（超时兜底）
  int32_t await_bbox_min_x = 0, await_bbox_min_y = 0;  // 等待期累计 damage 并集（未累计时 max<min）
  int32_t await_bbox_max_x = -1, await_bbox_max_y = -1;
  uint32_t old_content_w = 0, old_content_h = 0;  // 等待期 backing 中已填充有效内容的矩形
                                                  // （XCopyArea 自旧帧的左上重叠区，非垃圾）
  GC copy_gc = 0;                                 // XCopyArea 复用 GC（Start 创建，Stop 释放）
  int import_failures = 0;                        // 连续导入失败计数（用于一次性响亮日志）
  bool placeholder_logged = false;                // 白底占位分支一次性打点（验证路径命中）
  EGLDisplay engine_dpy = EGL_NO_DISPLAY;         // 首次导入时缓存的引擎显示句柄

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
  impl_->copy_gc = XCreateGC(impl_->dpy, impl_->webview_xwin, 0, nullptr);

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
    ds->on_damage = [this](uint32_t w, uint32_t h, bool covers_full, int32_t min_x, int32_t min_y,
                           int32_t max_x, int32_t max_y) {
      PresentOnce(w, h, covers_full, min_x, min_y, max_x, max_y);
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
    if (impl_->copy_gc != 0) {
      XFreeGC(impl_->dpy, impl_->copy_gc);
      impl_->copy_gc = 0;
    }
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

// 见头文件注释：累计本批 damage 进 await_bbox 并集并判定是否铺满目标 drawable。
bool WebKitGpuCapture::AccumulateAwaitCoverage(int32_t ev_min_x, int32_t ev_min_y,
                                               int32_t ev_max_x, int32_t ev_max_y, uint32_t w,
                                               uint32_t h) {
  if (ev_max_x >= ev_min_x && ev_max_y >= ev_min_y) {
    if (impl_->await_bbox_max_x < impl_->await_bbox_min_x) {  // 第一批
      impl_->await_bbox_min_x = ev_min_x;
      impl_->await_bbox_min_y = ev_min_y;
    } else {
      impl_->await_bbox_min_x = std::min(impl_->await_bbox_min_x, ev_min_x);
      impl_->await_bbox_min_y = std::min(impl_->await_bbox_min_y, ev_min_y);
    }
    impl_->await_bbox_max_x = std::max(impl_->await_bbox_max_x, ev_max_x);
    impl_->await_bbox_max_y = std::max(impl_->await_bbox_max_y, ev_max_y);
  }
  // 有效区域 = 拷贝的旧内容矩形 ∪ 跨批 damage 并集（联合包围盒阶梯近似）
  const bool bbox_valid = impl_->await_bbox_max_x >= impl_->await_bbox_min_x;
  const int32_t eff_max_x = std::max<int32_t>(bbox_valid ? impl_->await_bbox_max_x : 0,
                                              static_cast<int32_t>(impl_->old_content_w));
  const int32_t eff_max_y = std::max<int32_t>(bbox_valid ? impl_->await_bbox_max_y : 0,
                                              static_cast<int32_t>(impl_->old_content_h));
  return eff_max_x >= int32_t(w) && eff_max_y >= int32_t(h);
}

void WebKitGpuCapture::PresentOnce(uint32_t ev_w, uint32_t ev_h, bool ev_covers_full,
                                   int32_t ev_min_x, int32_t ev_min_y, int32_t ev_max_x,
                                   int32_t ev_max_y) {
  if (impl_ == nullptr || !active_ || !impl_->on_frame_available) {
    return;
  }
  // 节流：damage 风暴下限制 present 频率（~125Hz 上限）
  int64_t now = g_get_monotonic_time();
  if (now - impl_->last_present_us < 8000) {
    // 被丢弃批次的 damage 已在 X 侧消费完毕，守门期若跳过累计即永久丢失覆盖率：
    // 多批渐进重绘凑不齐「铺满 drawable」→ 首帧门永不开启（画面停在占位帧，
    // 且引擎逐帧告警）。故节流分支仍须记账；恰好在此刻铺满则放行本批，
    // 免得铺齐后还要空等下一次 damage。
    const bool same_generation = impl_->awaiting_full_repaint && ev_w != 0 && ev_h != 0 &&
                                 impl_->await_w == ev_w && impl_->await_h == ev_h;
    if (!same_generation ||
        !AccumulateAwaitCoverage(ev_min_x, ev_min_y, ev_max_x, ev_max_y, ev_w, ev_h)) {
      return;
    }
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
  // resize/首帧脏帧守门：几何与上次 present 不同 = backing 刚被服务端重分配，
  // 未重绘区域是未初始化显存，直接 present 即脏帧（初次载入/拖拽实测）。
  //
  // 等待策略（RCA：拖拽冻结 + 初次载入脏帧，三次迭代）：
  //  1. 首帧（无任何已交付帧）：只能等 WebKit 重绘完成——跨批累计 damage
  //     并集覆盖全 drawable 才放行（渐进分块渲染凑不齐单批全幅），不设超时
  //     兜底（黑屏优于显存噪声）。
  //  2. resize 过渡（有旧帧）：旧帧内容 + 边缘色填充把新 backing 填满后
  //     立即放行（零等待，每步出帧）；WebKit 全幅重绘随后到达，作为稳态帧
  //     覆盖。等待/超时仅剩拷贝失败的回退路径。
  // 注意：等待期间 pending 仍为旧值，后续事件依旧满足「几何≠pending」，
  // 必须用 await_w/h 判定是否已在等同一目标尺寸，否则重绘帧永远进不了
  // 放行分支（实测死锁）。等待中丢弃的 px 别名必须释放（实测泄漏点）。
  const bool resized_since_present = (impl_->pending_w != w || impl_->pending_h != h);
  const bool awaiting_this_size =
      impl_->awaiting_full_repaint && impl_->await_w == w && impl_->await_h == h;
  bool present_now;
  if (resized_since_present && !awaiting_this_size) {
    // 新 backing 世代：重置累计
    impl_->awaiting_full_repaint = true;
    impl_->await_w = w;
    impl_->await_h = h;
    impl_->await_start_us = now;
    impl_->await_bbox_min_x = 0;
    impl_->await_bbox_min_y = 0;
    impl_->await_bbox_max_x = -1;
    impl_->await_bbox_max_y = -1;
    impl_->old_content_w = 0;
    impl_->old_content_h = 0;
    // 拷贝源：引擎正在展示的 bound 优先，pending 兜底（锁内快照，锁外 X 调用）
    Pixmap src = 0;
    uint32_t sw = 0, sh = 0;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      if (impl_->bound_pixmap != 0) {
        src = impl_->bound_pixmap;
        sw = impl_->bound_w;
        sh = impl_->bound_h;
      } else if (impl_->pending_pixmap != 0) {
        src = impl_->pending_pixmap;
        sw = impl_->pending_w;
        sh = impl_->pending_h;
      }
    }
    if (src == 0) {
      // 首帧（无任何已交付帧，无可拷贝的旧内容）：只能等 WebKit 重绘完成
      // ——跨批累计 damage 并集覆盖全 drawable 才放行（超时兜底见下）。
      // 本批与新世代几何同源，覆盖率当场计入（丢弃即永久丢失）。
      debugLog("WebKitGpuCapture: await full repaint " + std::to_string(w) + "x" +
               std::to_string(h) + " (first frame, no old content)");
      if (!AccumulateAwaitCoverage(ev_min_x, ev_min_y, ev_max_x, ev_max_y, w, h)) {
        XFreePixmap(impl_->dpy, px);  // 等待期间不需要该别名（修复泄漏）
        return;
      }
      impl_->awaiting_full_repaint = false;
      debugLog(std::string("WebKitGpuCapture: release frame repaint-complete after ") +
               std::to_string((now - impl_->await_start_us) / 1000) +
               "ms (batch_full=" + (ev_covers_full ? "1" : "0") + " first-frame " +
               std::to_string(w) + "x" + std::to_string(h) + ")");
      present_now = true;
    } else {
      // resize 过渡：旧帧内容填充新 backing，消除「等整幅重绘」的拖拽冻结
      // （实测 20 步拖拽 1.2s 零帧交付——WebKit 全幅重绘 ~60-100ms 赶不上
      // 65ms 步进，在途重绘 damage 被下一步重置吞掉）。两步填充后 backing
      // 全域有效，本帧立即放行；WebKit 全幅重绘随后到达，作为稳态帧覆盖：
      //  1. XCopyArea：左上重叠区填旧帧内容（缩小方向即全域）
      //  2. XFillRectangle：增量区（右条+下条）用旧帧右下角像素色填充
      //     （自适应深/浅色页面；取色失败回退白色）
      const uint32_t cw = std::min(sw, w);
      const uint32_t ch = std::min(sh, h);
      unsigned long fill_pixel = 0xFFFFFFFF;
      bool fill_ok = false;
      {
        ScopedXErrors guard(impl_->dpy);
        if (impl_->copy_gc == 0) {
          impl_->copy_gc = XCreateGC(impl_->dpy, px, 0, nullptr);
        }
        XCopyArea(impl_->dpy, src, px, impl_->copy_gc, 0, 0, cw, ch, 0, 0);
        if (guard.count() == 0) {
          impl_->old_content_w = cw;
          impl_->old_content_h = ch;
          if (w > cw || h > ch) {
            // 取旧帧右下角单像素作为增量区填充色（深/浅色页面自适应）
            XImage* img = XGetImage(impl_->dpy, src, static_cast<int>(sw) - 1,
                                    static_cast<int>(sh) - 1, 1, 1, AllPlanes, ZPixmap);
            if (img != nullptr) {
              fill_pixel = XGetPixel(img, 0, 0);
              XDestroyImage(img);
            }
            XSetForeground(impl_->dpy, impl_->copy_gc, fill_pixel);
            if (w > cw) {
              XFillRectangle(impl_->dpy, px, impl_->copy_gc, static_cast<int>(cw), 0, w - cw, h);
            }
            if (h > ch) {
              XFillRectangle(impl_->dpy, px, impl_->copy_gc, 0, static_cast<int>(ch), cw, h - ch);
            }
          }
          fill_ok = guard.count() == 0;
        }
      }
      if (fill_ok) {
        // backing 全域有效（旧内容 + 填充边），立即放行，不等重绘
        impl_->awaiting_full_repaint = false;
        present_now = true;
      } else {
        // 拷贝/填充失败（X 错误）：backing 有垃圾，退回等待重绘（250ms 兜底）
        debugLog("WebKitGpuCapture: resize fill failed, fallback to await repaint");
        // 本批同样属于新世代，覆盖率当场计入
        AccumulateAwaitCoverage(ev_min_x, ev_min_y, ev_max_x, ev_max_y, w, h);
        XFreePixmap(impl_->dpy, px);
        return;
      }
    }
  } else if (impl_->awaiting_full_repaint) {
    // 守门等待中（仅首帧与填充失败回退进入）：跨批累计本批 damage 覆盖率。
    // 有效区域 = 拷贝的旧内容矩形 ∪ WebKit damage 并集，覆盖全 drawable 即
    // 重绘完成（联合包围盒阶梯近似：拷贝矩形自 (0,0) 起，增量 damage 补
    // 右上/右下，中间漏画的罕见场景由超时兜底收敛）。
    const bool effective_covers_full =
        AccumulateAwaitCoverage(ev_min_x, ev_min_y, ev_max_x, ev_max_y, w, h);
    // 超时兜底分级：
    //  - 尚无任何帧交付（首帧，无拷贝源）：1.2s——到点把并集外未重绘区刷白
    //    放行。原「不设超时」策略下，覆盖率一旦因故凑不齐就永不出帧（画面
    //    停在占位帧且引擎逐帧告警），白底与 WebKit 背景色一致，观感可控。
    //  - 已有旧帧（resize 过渡）：250ms——兜底放行的帧此时也已无垃圾
    //    （拷贝旧内容 + 部分新内容），只防 WebProcess 卡死类极端场景。
    const bool have_old_frame = impl_->pending_w != 0;
    if (!ev_covers_full && !effective_covers_full) {
      const int64_t budget_us = have_old_frame ? 250000 : 1200000;
      if (now - impl_->await_start_us < budget_us) {
        XFreePixmap(impl_->dpy, px);
        return;  // 等待预算未用完，继续等全幅重绘
      }
      if (!have_old_frame) {
        // 只刷并集之外（未初始化显存），已重绘内容原样保留
        const bool bbox_valid = impl_->await_bbox_max_x >= impl_->await_bbox_min_x;
        const int32_t bw = static_cast<int32_t>(w);
        const int32_t bh = static_cast<int32_t>(h);
        const int32_t bx0 =
            bbox_valid ? std::max<int32_t>(0, std::min(impl_->await_bbox_min_x, bw)) : 0;
        const int32_t by0 =
            bbox_valid ? std::max<int32_t>(0, std::min(impl_->await_bbox_min_y, bh)) : 0;
        const int32_t bx1 =
            bbox_valid ? std::max<int32_t>(0, std::min(impl_->await_bbox_max_x, bw)) : 0;
        const int32_t by1 =
            bbox_valid ? std::max<int32_t>(0, std::min(impl_->await_bbox_max_y, bh)) : 0;
        ScopedXErrors guard(impl_->dpy);
        if (impl_->copy_gc == 0) {
          impl_->copy_gc = XCreateGC(impl_->dpy, px, 0, nullptr);
        }
        XSetForeground(impl_->dpy, impl_->copy_gc, 0xFFFFFFFF);
        if (bx0 > 0) {
          XFillRectangle(impl_->dpy, px, impl_->copy_gc, 0, 0, bx0, bh);
        }
        if (by0 > 0) {
          XFillRectangle(impl_->dpy, px, impl_->copy_gc, 0, 0, bw, by0);
        }
        if (bx1 < bw) {
          XFillRectangle(impl_->dpy, px, impl_->copy_gc, bx1, 0, bw - bx1, bh);
        }
        if (by1 < bh) {
          XFillRectangle(impl_->dpy, px, impl_->copy_gc, 0, by1, bw, bh - by1);
        }
      }
    }
    const bool timed_out = !ev_covers_full && !effective_covers_full;
    impl_->awaiting_full_repaint = false;
    debugLog(std::string("WebKitGpuCapture: release frame ") +
             (timed_out ? "TIMEOUT" : "repaint-complete") + " after " +
             std::to_string((now - impl_->await_start_us) / 1000) +
             "ms (batch_full=" + (ev_covers_full ? "1" : "0") +
             " effective_full=" + (effective_covers_full ? "1" : "0") +
             " old_content=" + std::to_string(impl_->old_content_w) + "x" +
             std::to_string(impl_->old_content_h) + ")");
    present_now = true;
  } else {
    present_now = true;  // 稳态（几何未变且非等待）
  }
  if (!present_now) {
    return;
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
  bool first_frame_pending = false;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->pending_pixmap == 0) {
      if (impl_->bound_pixmap != 0) {
        *out_width = impl_->bound_w;
        *out_height = impl_->bound_h;
        return true;
      }
      first_frame_pending = true;
    }
  }
  if (first_frame_pending) {
    // 从未出帧（宿主延迟映射期 / 首帧守门期）：交付白底占位纹理。
    // 此处若报失败，引擎对每次合成 g_warning("no frame yet")——输入框聚焦时
    // 光标闪烁逐帧合成即刷屏。真帧到达后 EGLImage 整体替换纹理存储。
    if (!EnsurePlaceholderTexture(texture, error)) {
      return false;
    }
    if (!impl_->placeholder_logged) {
      impl_->placeholder_logged = true;
      debugLog("WebKitGpuCapture: no frame yet, bound white placeholder");
    }
    *out_width = 1;
    *out_height = 1;
    return true;
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
