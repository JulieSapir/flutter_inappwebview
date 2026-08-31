#include "inappwebview_gpu_texture.h"

#include <epoxy/gl.h>

#include <cstring>

#include "../utils/gl_context.h"
#include "../utils/log.h"
#include "webkit_gpu_capture.h"

// 私有结构
struct _InAppWebViewGpuTexture {
  FlTextureGL parent_instance;

  // 弱引用：生命周期归 InAppWebView（纹理注销先于捕获器销毁）
  flutter_inappwebview_plugin::WebKitGpuCapture* capture;

  GLuint texture_id;
  gboolean texture_initialized;
  uint32_t width;
  uint32_t height;
};

G_DEFINE_TYPE(InAppWebViewGpuTexture, inappwebview_gpu_texture, fl_texture_gl_get_type())

static gboolean inappwebview_gpu_texture_populate(FlTextureGL* texture, uint32_t* target,
                                                  uint32_t* name, uint32_t* out_width,
                                                  uint32_t* out_height, GError** error) {
  auto* self = INAPPWEBVIEW_GPU_TEXTURE(texture);

  if (self->capture == nullptr) {
    g_set_error(error, g_quark_from_static_string("InAppWebViewGpuTexture"), 1, "no capture");
    return FALSE;
  }

  // 引擎 raster 线程上下文校验（与 WPE EGL 纹理同款防线）
  if (!flutter_inappwebview_plugin::HasCurrentGLContext()) {
    if (self->texture_initialized && self->texture_id != 0) {
      *target = GL_TEXTURE_2D;
      *name = self->texture_id;
      *out_width = self->width > 0 ? self->width : 1;
      *out_height = self->height > 0 ? self->height : 1;
      return TRUE;
    }
    g_set_error(error, g_quark_from_static_string("InAppWebViewGpuTexture"), 2,
                "no current GL context - will retry");
    return FALSE;
  }

  if (!self->texture_initialized) {
    glGenTextures(1, &self->texture_id);
    self->texture_initialized = TRUE;
  }

  uint32_t w = 0, h = 0;
  if (!self->capture->BindPendingFrameToTexture(self->texture_id, &w, &h, error)) {
    return FALSE;
  }

  self->width = w;
  self->height = h;
  *target = GL_TEXTURE_2D;
  *name = self->texture_id;
  *out_width = w;
  *out_height = h;
  return TRUE;
}

static void inappwebview_gpu_texture_finalize(GObject* object) {
  auto* self = INAPPWEBVIEW_GPU_TEXTURE(object);
  self->capture = nullptr;  // 弱引用，不负责销毁
  G_OBJECT_CLASS(inappwebview_gpu_texture_parent_class)->finalize(object);
}

static void inappwebview_gpu_texture_class_init(InAppWebViewGpuTextureClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = inappwebview_gpu_texture_populate;
  G_OBJECT_CLASS(klass)->finalize = inappwebview_gpu_texture_finalize;
}

static void inappwebview_gpu_texture_init(InAppWebViewGpuTexture* self) {
  self->capture = nullptr;
  self->texture_id = 0;
  self->texture_initialized = FALSE;
  self->width = 0;
  self->height = 0;
}

InAppWebViewGpuTexture* inappwebview_gpu_texture_new(
    flutter_inappwebview_plugin::WebKitGpuCapture* capture) {
  auto* self = INAPPWEBVIEW_GPU_TEXTURE(g_object_new(INAPPWEBVIEW_TYPE_GPU_TEXTURE, nullptr));
  self->capture = capture;
  flutter_inappwebview_plugin::debugLog("InAppWebViewGpuTexture: created");
  return self;
}
