// im_fix.h - Xlib 坐标换算补偿（离屏宿主的 IME 候选框/根坐标语义修复）
//
// 背景 RCA：GPU 直通架构下 WebKitWebView 的 GdkWindow 挂在 override-redirect
// 离屏宿主上，宿主锚点定在显示器右下角内侧 1x1（screenX 修复的既定方案）。
// 输入法候选框定位链（fcitx5-gtk3 fcitximcontext.cpp:983 实锤）：
//   WebKit InputMethodFilterGtk caret rect → gtk_im_context_set_cursor_location
//   （相对 client window = WebKitWebView 的 GdkWindow）→ fcitx5 gtk module
//   gdk_window_get_root_coords(client_window) → daemon 在该根坐标画候选框。
// client window 的根坐标 = 离屏宿主锚点 → 候选框漂移到屏幕右下角，远离用户
// 视觉上的 webview 区域（真实桌面 fcitx5 截图客诉实锤）。
//
// 修复方式：Xlib 层符号覆盖 XTranslateCoordinates（exe 以 -rdynamic 导出后，
// libgdk/libgtk 内部对该符号的 PLT 引用命中本定义，实测覆盖）。仅对"已注册
// 的宿主窗口树 XID → 根窗口"的换算结果加 delta 补偿：
//   delta = (Flutter 窗口根原点 + webview 视口在窗口内偏移) - 离屏宿主根原点
// 使"宿主树内窗口的根坐标"等于用户视觉上的真实位置。fcitx5/ibus 的候选框
// 定位、GTK 菜单弹出等一切"宿主树 → 根"换算一并矫正。
//
// 注入要求：宿主应用可执行文件必须导出本文件符号（vai 的 linux runner 已配
// -rdynamic）。插件自身是静态链接进可执行文件的，无需 LD_PRELOAD。
// 注册表为空时零开销透传（除一次原子读）。
//
// 线程模型：GTK 主线程注册/注销；WebKitGpuCapture 的独立 X 连接线程可能并发
// 进入 hook，查表用互斥锁保护（表极小，开销可忽略）。

#ifndef FLUTTER_INAPPWEBVIEW_LINUX_IM_FIX_H_
#define FLUTTER_INAPPWEBVIEW_LINUX_IM_FIX_H_

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif

// 注册（或更新）一个 XID 的根坐标补偿。owner 用于实例隔离（多 WebView 实例
// 各自的宿主树互不干扰，注销按 owner 批量进行）。dx/dy 为加到换算结果上的
// 补偿量（设备像素）。
void vai_imfix_register(void* owner, Window xid, int dx, int dy);

// 注销某 owner 注册的全部 XID（WebView 实例销毁/重新注册前调用）。
void vai_imfix_unregister_owner(void* owner);

// 调试开关：设置环境变量 VAI_IMFIX_DEBUG=1 后，hook 命中与注册变化打印到
// stderr。默认静默（X 坐标换算是高频路径）。
void vai_imfix_set_debug(int enable);

#ifdef __cplusplus
}
#endif

#endif  // FLUTTER_INAPPWEBVIEW_LINUX_IM_FIX_H_
