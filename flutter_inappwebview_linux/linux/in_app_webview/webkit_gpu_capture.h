#ifndef FLUTTER_INAPPWEBVIEW_PLUGIN_WEBKIT_GPU_CAPTURE_H_
#define FLUTTER_INAPPWEBVIEW_PLUGIN_WEBKIT_GPU_CAPTURE_H_

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>

// WebKitGTK GPU 直通渲染（XComposite + EGLImage 零拷贝捕获）
//
// 管线（全链路 GPU，无 CPU 像素搬运）：
//   WebKit 合成器（WebProcess DMABuf 帧）
//   → UI 进程呈现到 webview 原生 X 窗口（GPU 显存 backing）
//   → XComposite redirect pixmap（同一 GPU buffer 的服务端别名）
//   → eglCreateImageKHR(EGL_NATIVE_PIXMAP_KHR) 导入（DRI3，零拷贝）
//   → glEGLImageTargetTexture2DOES 绑定为 FlTextureGL 纹理
//   → Flutter 引擎直接采样合成
//
// 对比已移除的 CPU 软渲染路径每帧 4 次搬运（快照 SHM → SIMD 转换 →
// 三缓冲 → staging → 上传），本路径每帧仅一次 EGL 导入
// （实测 ~1.1ms@1024x768），纹理内容由 GPU 直接产出。
//
// 帧驱动：XDamage 事件（独立 X 连接，GDK 会抽干默认连接队列导致丢事件，
// 见探针实证）→ NameWindowPixmap 取当前 backing 别名 → 通知纹理。
//
// 宿主要求：webview 必须位于真实（mapped）X 窗口内。GTK3 的
// GtkOffscreenWindow 不产生原生 X 窗口，无法捕获；因此宿主
// 使用 override-redirect 的 GTK_WINDOW_POPUP，定位到屏幕外。
//
// 能力检查（IsSupported + Start，任一不满足由调用方显式报错，无回退）：
//   - X11 后端（Wayland 会话无 X 窗口语义）
//   - Composite / Damage 扩展
//   - EGL_KHR_image_pixmap（Mesa X11 平台支持；NVIDIA 私有驱动未验证）
//
// 线程模型：
//   - 主线程：专用 X 连接的所有 Xlib 调用、damage 事件分发
//   - 引擎 raster 线程：BindPendingFrameToTexture（populate 回调）
//   - 两侧通过 pending/bound 槽位 + 互斥锁交接

namespace flutter_inappwebview_plugin {

class WebKitGpuCapture {
 public:
  WebKitGpuCapture();
  ~WebKitGpuCapture();

  WebKitGpuCapture(const WebKitGpuCapture&) = delete;
  WebKitGpuCapture& operator=(const WebKitGpuCapture&) = delete;

  // 能力检查（只读探测，不落地资源）。webview 需已 realize。
  static bool IsSupported(GtkWidget* webview_widget);

  // 启动捕获：独立 X 连接 + redirect + damage 事件源 + 首帧别名。
  bool Start(GtkWidget* webview_widget);
  void Stop();

  bool IsActive() const { return active_; }

  // 输出消费者（GPU 纹理）接入后 damage 才产帧。
  // 接入时立即补一次 present，保证首帧不空。
  void SetOnFrameAvailable(std::function<void()> callback);

  // 强制取当前内容别名并入队一帧（resize/scale 变化后由 RequestSnapshot 调用）。
  // ev_w/ev_h：damage 事件自带的 drawable 几何（服务端真值，与该帧 backing
  // 内容严格同源）；传 0 时退化为一次 XGetGeometry 取当前真值。
  // ev_covers_full：本批 damage 区域包围盒是否覆盖整个 drawable（WebKit 对
  // 新尺寸的全幅重绘标志）。
  // ev_min/max：本批 damage 包围盒（max<min 表示无 bbox 信息），供守门逻辑
  // 跨批累计并集判定「多批渐进重绘合起来铺满 drawable」。
  void PresentOnce(uint32_t ev_w = 0, uint32_t ev_h = 0, bool ev_covers_full = false,
                   int32_t ev_min_x = 0, int32_t ev_min_y = 0, int32_t ev_max_x = -1,
                   int32_t ev_max_y = -1);

  // === 引擎 raster 线程（FlTextureGL::populate）===

  // 若有 pending 别名：EGL 导入并绑定到 [texture]，输出尺寸，返回 true。
  // 无 pending：返回 false（纹理保留上帧内容，引擎按原样合成）。
  // 失败（EGL 导入被拒等）：返回 false 并置 error；连续失败会 errorLog 提示。
  bool BindPendingFrameToTexture(uint32_t texture, uint32_t* out_width, uint32_t* out_height,
                                 GError** error);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool active_ = false;

  // 守门期覆盖率累计（主线程专用，await_* 不参与跨线程交接故免锁）：把本批
  // damage 包围盒并入 await_bbox 并集，返回「并集 ∪ 已填充旧内容区」是否已
  // 覆盖 [w]x[h]。present 节流丢弃的批次同样要走这里，否则覆盖率永久丢失。
  bool AccumulateAwaitCoverage(int32_t ev_min_x, int32_t ev_min_y, int32_t ev_max_x,
                               int32_t ev_max_y, uint32_t w, uint32_t h);
};

}  // namespace flutter_inappwebview_plugin

#endif  // FLUTTER_INAPPWEBVIEW_PLUGIN_WEBKIT_GPU_CAPTURE_H_
