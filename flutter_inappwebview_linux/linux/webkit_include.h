// webkit_include.h - 统一 WebKit/JavaScriptCore 头文件引用
//
// 在 WebKitGTK 后端与 WPE WebKit 后端之间屏蔽头文件路径差异。
// 两个移植版共享同一套 WebKit2 GObject API（WebKitWebView、WebKitSettings 等），
// 仅头文件位置不同：
//   - WebKitGTK: <webkit2/webkit2.h>（pkg-config: webkit2gtk-4.1）
//   - WPE:       <wpe/webkit.h>     （pkg-config: wpe-webkit-2.0 / 1.1 / 1.0）
// JavaScriptCore 头路径 <jsc/jsc.h> 两者一致。
//
// 新代码请一律 include 本头文件，不要直接 include <wpe/webkit.h>。

#ifndef FLUTTER_INAPPWEBVIEW_PLUGIN_WEBKIT_INCLUDE_H_
#define FLUTTER_INAPPWEBVIEW_PLUGIN_WEBKIT_INCLUDE_H_

#ifdef HAVE_WEBKIT_GTK
#include <jsc/jsc.h>
#include <webkit2/webkit2.h>

// === WebKitGTK 4.1 与 WPE WebKit 的 API 差异收敛层 ===
// （已核实本机 webkit2gtk-4.1 2.52 头文件：WebContext 模型，无 NetworkSession）
//
// 1. NetworkSession → WebContext：
//    GTK 移植中 cookie/website-data/proxy/download 等能力由 WebKitWebContext
//    承担（WPE 2.40+ 将其重构进 WebKitNetworkSession）。用宏把 WPE 命名
//    重定向到 GTK 对应 API，call site 无需分散 ifdefs。
#define WebKitNetworkSession WebKitWebContext
#define webkit_network_session_get_default() webkit_web_context_get_default()
#define webkit_network_session_new_ephemeral() webkit_web_context_new_ephemeral()
#define webkit_web_view_get_network_session(v) webkit_web_view_get_context(v)
#define webkit_network_session_get_cookie_manager(s) webkit_web_context_get_cookie_manager(s)
#define webkit_network_session_get_website_data_manager(s) \
  webkit_web_context_get_website_data_manager(s)
#define webkit_network_session_set_proxy_settings(s, m, p) \
  webkit_website_data_manager_set_network_proxy_settings(  \
      webkit_web_context_get_website_data_manager(s), m, p)
// ITP（智能防跟踪）：WebKitGTK 4.1 无公开 API（能力差异，非回退）
#define webkit_network_session_set_itp_enabled(s, e) ((void)(s), (void)(e))

// 2. Rectangle：WPE 定义 WebKitRectangle，GTK 使用 GdkRectangle
#define WebKitRectangle GdkRectangle

// 3. Color：WPE 定义 WebKitColor（double r/g/b/a），GTK 使用 GdkRGBA（同字段序）
#define WebKitColor GdkRGBA

// 4. TLS 例外：NetworkSession → WebContext
#define webkit_network_session_allow_tls_certificate_for_host(c, cert, host) \
  webkit_web_context_allow_tls_certificate_for_host(c, cert, host)

// 5. Script message handler：WPE 的 register/unregister 带 world_name 参数，
//    GTK 老签名为 2 参（in_world 变体走 WebKitScriptWorld* 对象）。
//    本插件全部调用点 world_name 均为 nullptr，映射到 GTK 老签名。
inline gboolean webkit_compat_register_script_message_handler(WebKitUserContentManager* manager,
                                                              const gchar* name,
                                                              const gchar* world_name) {
  (void)world_name;
  return webkit_user_content_manager_register_script_message_handler(manager, name);
}
inline void webkit_compat_unregister_script_message_handler(WebKitUserContentManager* manager,
                                                            const gchar* name,
                                                            const gchar* world_name) {
  (void)world_name;
  webkit_user_content_manager_unregister_script_message_handler(manager, name);
}

// 3. Web 进程扩展目录：GTK 2.52 仍为 web_extensions_directory 命名
#define webkit_web_context_set_web_process_extensions_directory(c, d) \
  webkit_web_context_set_web_extensions_directory(c, d)

#else
#include <jsc/jsc.h>
#include <wpe/webkit.h>
#endif

#endif  // FLUTTER_INAPPWEBVIEW_PLUGIN_WEBKIT_INCLUDE_H_
