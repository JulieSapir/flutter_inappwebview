// webkit_include.h - 统一 WebKit/JavaScriptCore 头文件引用与 API 面收敛
//
// Linux 后端唯一后端为 WebKitGTK（webkit2gtk-4.1）。新代码请一律 include
// 本头文件，不要直接 include <webkit2/webkit2.h>。
//
// 收敛层：以下宏把 WPE / 新版 WebKit 的 API 名映射到 webkit2gtk-4.1 的
// 实际 API，使共享代码可以用统一名字书写。以 webkit2gtk-4.1（本机 2.52）
// 头文件为权威：
//   - GTK 4.1 API 面为 WebContext 模型，无 WebKitNetworkSession
//   - show-option-menu 信号用 GdkRectangle，背景色用 GdkRGBA
//   - 2.52 的 4.1 仍是 webkit_web_context_set_web_extensions_directory
//     （非 web*process*extensions）

#ifndef FLUTTER_INAPPWEBVIEW_PLUGIN_WEBKIT_INCLUDE_H_
#define FLUTTER_INAPPWEBVIEW_PLUGIN_WEBKIT_INCLUDE_H_

#include <jsc/jsc.h>
#include <webkit2/webkit2.h>

// --- NetworkSession → WebContext（GTK 4.1 为 WebContext 模型） ---
#define WebKitNetworkSession WebKitWebContext
#define webkit_web_view_get_network_session webkit_web_view_get_context
#define webkit_network_session_get_default webkit_web_context_get_default
#define webkit_network_session_new_ephemeral webkit_web_context_new_ephemeral
#define webkit_network_session_get_cookie_manager webkit_web_context_get_cookie_manager
#define webkit_network_session_get_website_data_manager webkit_web_context_get_website_data_manager
#define webkit_network_session_set_proxy_settings webkit_web_context_set_proxy_settings
#define webkit_network_session_allow_tls_certificate_for_host \
  webkit_web_context_allow_tls_certificate_for_host

// --- 基础类型 ---
#define WebKitRectangle GdkRectangle
#define WebKitColor GdkRGBA

// --- Web 扩展目录命名差异 ---
#define webkit_web_context_set_web_process_extensions_directory \
  webkit_web_context_set_web_extensions_directory

#endif  // FLUTTER_INAPPWEBVIEW_PLUGIN_WEBKIT_INCLUDE_H_
