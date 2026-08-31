#ifndef FLUTTER_INAPPWEBVIEW_PLUGIN_INAPPWEBVIEW_GPU_TEXTURE_H_
#define FLUTTER_INAPPWEBVIEW_PLUGIN_INAPPWEBVIEW_GPU_TEXTURE_H_

#include <flutter_linux/flutter_linux.h>

G_BEGIN_DECLS

// WebKitGTK GPU 直通纹理：FlTextureGL 子类。
// populate 时把 WebKitGpuCapture 的 pending X 窗口别名（redirect pixmap）
// 经 EGLImage 导入并绑定为本纹理内容，全程零 CPU 像素搬运。
// 捕获器由 InAppWebView 持有，纹理仅持弱引用（注销先于捕获器销毁）。
#define INAPPWEBVIEW_TYPE_GPU_TEXTURE (inappwebview_gpu_texture_get_type())

G_DECLARE_FINAL_TYPE(InAppWebViewGpuTexture, inappwebview_gpu_texture, INAPPWEBVIEW, GPU_TEXTURE,
                     FlTextureGL)

namespace flutter_inappwebview_plugin {
class WebKitGpuCapture;
}

InAppWebViewGpuTexture* inappwebview_gpu_texture_new(
    flutter_inappwebview_plugin::WebKitGpuCapture* capture);

G_END_DECLS

#endif  // FLUTTER_INAPPWEBVIEW_PLUGIN_INAPPWEBVIEW_GPU_TEXTURE_H_
