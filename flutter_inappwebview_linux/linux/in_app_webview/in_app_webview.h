#ifndef FLUTTER_INAPPWEBVIEW_PLUGIN_IN_APP_WEBVIEW_H_
#define FLUTTER_INAPPWEBVIEW_PLUGIN_IN_APP_WEBVIEW_H_

// InAppWebView implementation (WebKitGTK).
//
// 渲染采用 GPU 直通唯一管线（webkit_gpu_capture.*）：widget 挂 override-redirect
// 屏内原点 popup 宿主（手动 XComposite redirect 保证不可见），XDamage 驱动 +
// EGLImage 零拷贝。输入合成 GdkEvent（实现见 in_app_webview_gtk.cc）。
// 无 CPU 回退管线：能力不满足时显式报错。

#include <flutter_linux/flutter_linux.h>

// WebKit core includes (WebKitGTK)
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "../content_blocker/content_blocker_handler.h"
#include "../find_interaction/find_interaction_controller.h"
#include "../types/context_menu.h"
#include "../types/context_menu_popup.h"
#include "../types/find_session.h"
#include "../types/hit_test_result.h"
#include "../types/option_menu_popup.h"
#include "../types/ssl_certificate.h"
#include "../types/url_request.h"
#include "../types/user_script.h"
#include "../webkit_include.h"
#include "in_app_webview_settings.h"

namespace flutter_inappwebview_plugin {

// 宿主屏内锚点：origin=(0,0) 显示器右下角内侧 1x1（详见
// in_app_webview_gtk.cc 内注释）。实现于 in_app_webview_gtk.cc。
void HostAnchorPosition(int* x, int* y);

class InAppBrowser;
class InAppWebViewManager;
class PluginInstance;
class UserContentController;
class WebMessageChannel;
class WebMessageListener;
class WebViewChannelDelegate;
class WebKitGpuCapture;

struct InAppWebViewCreationParams {
  int64_t id;
  PluginInstance* plugin = nullptr;        // Plugin instance for accessing managers
  GtkWindow* gtkWindow = nullptr;          // Cached GTK window from manager
  FlView* flView = nullptr;                // Cached FlView for focus restoration
  InAppWebViewManager* manager = nullptr;  // Manager reference for multi-window support
  std::optional<std::shared_ptr<URLRequest>> initialUrlRequest;
  std::optional<std::string> initialFile;
  std::optional<std::string> initialData;
  std::optional<std::string> initialDataBaseUrl;
  std::optional<std::string> initialDataMimeType;
  std::optional<std::string> initialDataEncoding;
  std::shared_ptr<InAppWebViewSettings> initialSettings;
  std::optional<std::shared_ptr<ContextMenu>> contextMenu;
  std::optional<int64_t> windowId;          // For windows created via onCreateWindow
  WebKitWebView* relatedWebView = nullptr;  // For creating related WebViews (shares web process)
  std::vector<std::shared_ptr<UserScript>> initialUserScripts;  // User scripts to inject
  WebKitWebContext* webContext = nullptr;  // Custom WebKitWebContext from WebViewEnvironment
  // True when the widget is hosted inside a browser's GTK window (InAppBrowser):
  // skips the offscreen host and lets the browser own the widget hierarchy.
  bool hostInBrowserWindow = false;
};

// Pointer event kind (matches Dart side)
enum class WpePointerEventKind {
  Activate = 0,
  Down = 1,
  Enter = 2,
  Leave = 3,
  Up = 4,
  Update = 5,
  Cancel = 6
};

// (WpePointerButton 死枚举已删除：零使用，且 None 与 X11/X.h 的
//  "#define None 0L" 宏冲突，示例链路重编时触发 expected identifier)

/// InAppWebView - WPE WebKit based implementation
///
/// This class provides offscreen web rendering using WPE WebKit.
/// Unlike WebKitGTK, WPE doesn't require a GTK widget hierarchy
/// and can render directly to GPU textures via the FDO backend.
class InAppWebView {
 public:
  static constexpr const char* METHOD_CHANNEL_NAME_PREFIX =
      "com.pichillilorenzo/flutter_inappwebview_";

  InAppWebView(FlPluginRegistrar* registrar, FlBinaryMessenger* messenger, int64_t id,
               const InAppWebViewCreationParams& params);
  ~InAppWebView();

  int64_t id() const { return id_; }
  WebKitWebView* webview() const { return webview_; }
  WebViewChannelDelegate* channel_delegate() const { return channel_delegate_.get(); }
  FlPluginRegistrar* registrar() const { return registrar_; }

  // Attach/recreate the Dart method channel using the given [channel_id].
  void AttachChannel(FlBinaryMessenger* messenger, int64_t channel_id);

  // Attach/recreate the Dart method channel using a string-based channel ID.
  // This is used for HeadlessInAppWebView where the ID is a long string from Dart.
  void AttachChannel(FlBinaryMessenger* messenger, const std::string& channel_id,
                     const bool is_full_channel_name);

  int64_t channel_id() const { return channel_id_; }
  const std::string& string_channel_id() const { return string_channel_id_; }

  // Navigation methods
  void loadUrl(const std::string& url);
  void loadUrl(const std::shared_ptr<URLRequest>& urlRequest);
  void loadData(const std::string& data, const std::string& mime_type, const std::string& encoding,
                const std::string& base_url);
  void loadFile(const std::string& asset_file_path);
  void postUrl(const std::string& url, const std::vector<uint8_t>& postData);
  void reload();
  void reloadFromOrigin();
  void goBack();
  void goForward();
  void goBackOrForward(int steps);
  bool canGoBack() const;
  bool canGoForward() const;
  bool canGoBackOrForward(int steps) const;
  void stopLoading();
  bool isLoading() const;

  // Navigation history
  FlValue* getCopyBackForwardList() const;

  // Getters
  std::optional<std::string> getUrl() const;
  std::optional<std::string> getTitle() const;
  int64_t getProgress() const;

  // TLS/SSL certificate
  // Returns the SSL certificate info for the current page (or nullopt if not HTTPS)
  std::optional<SslCertificate> getCertificate() const;

  // Hit test result
  // Returns the last hit test result from mouse-target-changed signal
  HitTestResult getHitTestResult() const;

  // JavaScript execution
  void evaluateJavascript(const std::string& source, const std::optional<std::string>& worldName,
                          std::function<void(const std::optional<std::string>&)> callback);
  void callAsyncJavaScript(const std::string& functionBody, const std::string& argumentsJson,
                           const std::vector<std::string>& argumentKeys,
                           const std::optional<std::string>& worldName,
                           std::function<void(const std::string&)> callback);
  void injectJavascriptFileFromUrl(const std::string& urlFile);
  void injectCSSCode(const std::string& source);
  void injectCSSFileFromUrl(const std::string& urlFile);

  // User scripts
  void addUserScript(std::shared_ptr<UserScript> userScript);
  void removeUserScriptAt(size_t index, UserScriptInjectionTime injectionTime);
  void removeUserScriptsByGroupName(const std::string& groupName);
  void removeAllUserScripts();

  // Web Message Listener
  void addWebMessageListener(const std::string& jsObjectName,
                             const std::vector<std::string>& allowedOriginRules);

  // Web Message Channel
  void createWebMessageChannel(std::function<void(const std::optional<std::string>&)> callback);
  void postWebMessage(const std::string& messageData, const std::string& targetOrigin,
                      int64_t messageType);
  void setWebMessageCallback(const std::string& channelId, int portIndex);
  void postWebMessageOnPort(const std::string& channelId, int portIndex,
                            const std::string& messageData, int64_t messageType);
  void closeWebMessagePort(const std::string& channelId, int portIndex);
  void disposeWebMessageChannel(const std::string& channelId);
  WebMessageChannel* getWebMessageChannel(const std::string& channelId) const;

  // HTML content
  void getHtml(std::function<void(const std::optional<std::string>&)> callback);

  // Screenshot - captures the current visible content as PNG data
  void takeScreenshot(std::function<void(const std::optional<std::vector<uint8_t>>&)> callback);

  // Session state - save and restore navigation state
  std::optional<std::vector<uint8_t>> saveState() const;
  bool restoreState(const std::vector<uint8_t>& stateData);

  // Zoom
  double getZoomScale() const;
  void setZoomScale(double zoomScale);

  // Scroll
  void scrollTo(int64_t x, int64_t y, bool animated);
  void scrollBy(int64_t x, int64_t y, bool animated);
  void getScrollX(std::function<void(int64_t)> callback);
  void getScrollY(std::function<void(int64_t)> callback);
  void canScrollVertically(std::function<void(bool)> callback);
  void canScrollHorizontally(std::function<void(bool)> callback);

  // Content dimensions (async - via JavaScript)
  void getContentHeight(std::function<void(int64_t)> callback);
  void getContentWidth(std::function<void(int64_t)> callback);

  // Find interaction controller (now managed separately)
  FindInteractionController* findInteractionController() const {
    return findInteractionController_.get();
  }

  // Settings
  std::shared_ptr<InAppWebViewSettings> settings() const { return settings_; }
  FlValue* getSettings() const;
  void setSettings(const std::shared_ptr<InAppWebViewSettings> newSettings,
                   FlValue* newSettingsMap = nullptr);

  // User content controller
  UserContentController* userContentController() const { return user_content_controller_.get(); }

  // Size management
  void setSize(int width, int height);
  void setScaleFactor(double scale_factor);

  // Focus/Activity state management
  void setFocused(bool focused);
  void setVisible(bool visible);
  uint32_t getActivityState() const;

  // Refresh rate management（WebKitGTK 无协商 API，仅通道协议缓存）
  void setTargetRefreshRate(uint32_t rate);
  uint32_t getTargetRefreshRate() const;

  // Screen scale management
  double getScreenScale() const;
  void setScreenScale(double scale);

  // Visibility management
  bool isVisible() const;

  // Fullscreen control（WebKitGTK 走 enter/leave-fullscreen 信号）
  void requestEnterFullscreen();
  void requestExitFullscreen();
  bool isInFullscreen() const { return is_fullscreen_; }

  // Pointer lock support - for games/immersive apps（WebKitGTK 无公开 API，返回 false）
  void setPointerLockHandler(std::function<bool(bool)> handler);
  bool requestPointerLock();
  bool requestPointerUnlock();

  // Input handling
  void SetTextureOffset(double x, double y);

  // IME 根坐标补偿（im_fix）：注册宿主窗口树 XID 并维护 delta，使输入法
  // 候选框/GTK 菜单等“宿主树 → 根”换算结果等于用户视觉上的真实位置。
  // 仅 X11 离屏宿主路径有意义，其它后端为空实现或无调用。
  void RefreshImFixRegistration();
  void SetCursorPos(double x, double y);
  void SetPointerButton(int kind, int button, int clickCount = 1);
  // dx/dy：Flutter 逻辑像素（= 原生 GDK 滚动单位 × 53）。
  // precise：false=鼠标滚轮、true=触控板 pan。WebKitGTK 对两条通道的
  // 单位→像素步长不同，必须带通道信息才能还原原生手感（见 GtkSetScrollDelta）。
  void SetScrollDelta(double dx, double dy, bool precise);
  void SendKeyEvent(int type, int64_t keyCode, int scanCode, int modifiers,
                    const std::string& characters);
  void SendTouchEvent(int type, int id, double x, double y,
                      const std::vector<std::tuple<int, double, double, int>>& touchPoints);

  // Texture pixel buffer access (called by texture classes)
  // 已随 CPU 回退管线移除：GPU 直通纹理由 webkit_gpu_capture 直接提供

  // Frame available callback (called when new frame is ready)
  void SetOnFrameAvailable(std::function<void()> callback);

  // Cursor change callback
  void SetOnCursorChanged(std::function<void(const std::string&)> callback);

  // Progress change callback (for InAppBrowser progress bar)
  void SetOnProgressChanged(std::function<void(double)> callback);

  // Navigation state change callback (for InAppBrowser back/forward buttons)
  void SetOnNavigationStateChanged(std::function<void()> callback);

  // InAppBrowser delegate (when this WebView is embedded in an InAppBrowser)
  // Used by WebViewChannelDelegate to forward browser-specific methods
  void setInAppBrowserDelegate(InAppBrowser* browser) { inAppBrowserDelegate_ = browser; }
  InAppBrowser* getInAppBrowserDelegate() const { return inAppBrowserDelegate_; }

  // Called from Dart when shouldOverrideUrlLoading decision is made
  void OnShouldOverrideUrlLoadingDecision(int64_t decision_id, bool allow);

  // Context menu methods
  // Context menu methods
  // Show the native GTK context menu using pending WebKit menu and custom items
  void ShowNativeContextMenu();
  // Hide and cleanup any visible context menu
  void HideContextMenu();

  // Color picker methods (for <input type="color"> support in WPE)
  // Show the native color picker popup with optional predefined colors and alpha support
  void ShowColorPicker(const std::string& initialColor, int x, int y,
                       const std::vector<std::string>& predefinedColors = {},
                       bool alphaEnabled = false, const std::string& colorSpace = "limited-srgb");
  // Hide and cleanup any visible color picker
  void HideColorPicker();
  // Hide and cleanup any visible file chooser dialog
  void HideFileChooser();
  // Hide and cleanup any visible option menu (HTML <select>)
  void HideOptionMenu();

  // Date picker methods (for <input type="date/time"> support in WPE)
  // Show the native date/time picker dialog
  void ShowDatePicker(const std::string& inputType, const std::string& value,
                      const std::string& min, const std::string& max, const std::string& step,
                      int x, int y);
  // Hide and cleanup any visible date picker
  void HideDatePicker();

  // Resolve an internal handler's Promise with a JSON result via WebKitScriptMessageReply
  // Used by color/date picker dialogs to send the result back to JavaScript (works for iframes)
  void ResolveInternalHandlerWithReply(WebKitScriptMessageReply* reply,
                                       const std::string& jsonResult);

  // JavaScript bridge handler using with_reply API (enables iframe support)
  // Returns true if handled, false otherwise
  bool handleScriptMessageWithReply(const std::string& body, WebKitScriptMessageReply* reply);

  // Reject an internal handler's Promise with an error message via WebKitScriptMessageReply
  void RejectInternalHandlerWithReply(WebKitScriptMessageReply* reply,
                                      const std::string& errorMessage);

  // Hide all custom popups (context menu, color picker, file chooser, option menu, etc.)
  // Use this when the webview state changes (resize, scroll, load, focus loss, etc.)
  void HideAllPopups();

  // Clipboard operations (syncs WPE WebKit clipboard with system clipboard)
  void copyToClipboard();
  void cutToClipboard();
  void pasteFromClipboard();
  void pasteAsPlainText();
  void copyTextToClipboard(const std::string& text);  // Copy arbitrary text to both clipboards
  void getSelectedText(std::function<void(const std::optional<std::string>&)> callback);
  void isSecureContext(std::function<void(bool)> callback);

  // Media playback control
  void pauseAllMediaPlayback();
  void setAllMediaPlaybackSuspended(bool suspended);
  void closeAllMediaPresentations();
  void requestMediaPlaybackState(std::function<void(int)> callback);

  // Media capture state (camera and microphone)
  int getCameraCaptureState() const;
  void setCameraCaptureState(int state);
  int getMicrophoneCaptureState() const;
  void setMicrophoneCaptureState(int state);

  // Theme color (from <meta name="theme-color"> tag)
  std::optional<std::string> getMetaThemeColor() const;

  // Audio state (mute and playback)
  bool isPlayingAudio() const;
  bool isMuted() const;
  void setMuted(bool muted);

  // Web process control
  void terminateWebProcess();

  // Focus control
  bool clearFocus();
  bool requestFocus();

  // Web archive (save page to file)
  void saveWebArchive(const std::string& filePath, bool autoname,
                      std::function<void(const std::optional<std::string>&)> callback);

  // Editing commands (WebKit editing commands)
  void selectAll();
  void undo();
  void redo();
  void insertImage(const std::string& imageUri);
  void createLink(const std::string& linkUri);

  // Check if the WebKit backend is available on the system
  // （方法名保留 WPE 字样仅为 Dart 侧通道协议兼容，恒为 true）
  static bool IsWpeWebKitAvailable();

  // GPU 直通是否激活（CustomPlatformView 据此选择纹理类型）。
  bool IsGpuCaptureActive() const;
  // 捕获器弱访问（供 GPU 纹理构造）。
  WebKitGpuCapture* gpu_capture() const;
  // GPU 纹理注册完成后接入帧输出（设置 damage→mark 回调并补首帧）。
  void AttachGpuCaptureOutput();

  // === Multi-Window Support ===

  // Set the window ID for this webview (used in window.open scenarios)
  void setWindowId(int64_t windowId) { window_id_ = windowId; }

  // Get the window ID (null if not set)
  std::optional<int64_t> getWindowId() const { return window_id_; }

  // Initialize the window ID JavaScript variable in the webview
  // This injects JS to set window._flutter_inappwebview_windowId
  void initializeWindowIdJS();

  // Get the GTK window (for focus restoration after popup dialogs)
  GtkWindow* getGtkWindow() const { return gtk_window_; }

  // Get the FlView (for focus restoration after popup dialogs)
  FlView* getFlView() const { return fl_view_; }

 private:
  PluginInstance* plugin_ = nullptr;  // Plugin instance for accessing managers
  FlPluginRegistrar* registrar_ = nullptr;
  FlBinaryMessenger* messenger_ = nullptr;  // Cached messenger from constructor
  GtkWindow* gtk_window_ = nullptr;         // Cached GTK window for context menu display
  FlView* fl_view_ = nullptr;               // Cached FlView for focus restoration
  InAppWebViewManager* manager_ = nullptr;  // Manager reference for multi-window support
  int64_t id_ = 0;
  int64_t channel_id_ = -1;
  std::string string_channel_id_;  // String-based channel ID for headless webviews

  // Settings
  std::shared_ptr<InAppWebViewSettings> settings_;

  // Context menu configuration
  std::shared_ptr<ContextMenu> context_menu_config_;

  // WPE WebKit view
  WebKitWebView* webview_ = nullptr;

  // === 离屏宿主与 GPU 直通 ===
  // override-redirect popup 宿主（屏内原点定位）：XComposite 捕获需要真实原生
  // X 窗口，GTK3 未 map 宿主不产生 X 窗口。宿主 GdkWindow 原点是 WebKitGTK
  // window.screenX/screenY 的取值来源，必须落在真实屏幕内，否则依赖该
  // 语义的站点（testufo 刷新率测试等）会误判"窗口不在主显示器"。
  // 屏内隐形用 XShape bounding/input 全空保证（见 HideHostWindow）。
  GtkWindow* gtk_host_window_ = nullptr;

  // GPU 直通捕获（XComposite redirect + EGLImage 零拷贝）。构造时能力检查
  // 失败则为空（唯一渲染管线不可用，显式报错，无回退）。
  std::unique_ptr<WebKitGpuCapture> gpu_capture_ = nullptr;
  // 延迟映射状态：InitGtkHost 只 realize 不 map；MapHostNow 首次调用后置 true。
  bool host_mapped_ = false;
  guint map_failsafe_source_id_ = 0;

  // GTK signal handlers
  gulong gtk_scale_handler_id_ = 0;         // notify::scale-factor on the webview widget
  gulong gtk_window_scale_handler_id_ = 0;  // notify::scale-factor on the gtk window
  gulong gtk_window_configure_handler_id_ = 0;  // configure-event on the gtk window (IME delta refresh)

  // View dimensions
  int width_ = 800;
  int height_ = 600;
  double scale_factor_ = 1.0;

  // Channel delegate
  std::unique_ptr<WebViewChannelDelegate> channel_delegate_;

  // User content controller
  std::unique_ptr<UserContentController> user_content_controller_;

  // Find interaction controller
  std::unique_ptr<FindInteractionController> findInteractionController_;

  // Content blocker handler for Safari-style content blocking rules
  std::unique_ptr<ContentBlockerHandler> content_blocker_handler_;

  // Web message channels (for WebMessageChannel support)
  std::map<std::string, std::unique_ptr<WebMessageChannel>> web_message_channels_;

  // Web message listeners (for WebMessageListener support - federated plugin pattern)
  // Key is jsObjectName, value is the WebMessageListener
  std::map<std::string, std::unique_ptr<WebMessageListener>> web_message_listeners_;

  // Initial user scripts from params
  std::vector<std::shared_ptr<UserScript>> initial_user_scripts_;

  // JavaScript bridge secret for security
  std::string js_bridge_secret_;

  // Window ID for multi-window support
  std::optional<int64_t> window_id_;

  // Flag to track if javaScriptBridgeEnabled
  bool java_script_bridge_enabled_ = true;

  // Pending policy decisions
  std::map<int64_t, WebKitPolicyDecision*> pending_policy_decisions_;
  int64_t next_decision_id_ = 0;

  // Pending script dialogs
  std::map<int64_t, WebKitScriptDialog*> pending_script_dialogs_;
  int64_t next_dialog_id_ = 0;

  // Pending permission requests
  std::map<int64_t, WebKitPermissionRequest*> pending_permission_requests_;
  int64_t next_permission_id_ = 0;

  // Pending authentication requests
  std::map<int64_t, WebKitAuthenticationRequest*> pending_auth_requests_;
  int64_t next_auth_id_ = 0;

  // Pending custom scheme requests (for async handling)
  std::map<WebKitURISchemeRequest*, int64_t> pending_custom_scheme_requests_;

  // Frame available callback
  std::function<void()> on_frame_available_;

  // Cursor change callback
  std::function<void(const std::string&)> on_cursor_changed_;
  std::string last_cursor_name_ = "default";

  // Progress change callback (for InAppBrowser)
  std::function<void(double)> on_progress_changed_;

  // Navigation state change callback (for InAppBrowser back/forward buttons)
  std::function<void()> on_navigation_state_changed_;

  // InAppBrowser delegate (when embedded in an InAppBrowser)
  // This allows WebViewChannelDelegate to forward browser-specific method calls
  InAppBrowser* inAppBrowserDelegate_ = nullptr;

  // Last hit test result from mouse-target-changed signal
  // Used by getHitTestResult() to return the current element under the cursor
  WebKitHitTestResult* last_hit_test_result_ = nullptr;

  // Disposing flag to prevent callbacks during destruction
  std::atomic<bool> is_disposing_{false};

  // Mouse state
  double cursor_x_ = 0;
  double cursor_y_ = 0;
  uint32_t button_state_ = 0;
  uint32_t current_modifiers_ = 0;  // Current keyboard modifiers (shift, ctrl, alt, meta)


  // Progress tracking
  double last_progress_ = 0.0;

  // Media capture state tracking (for onCameraCaptureStateChanged/onMicrophoneCaptureStateChanged)
  int last_camera_capture_state_ = 0;      // WebKitMediaCaptureState: NONE=0, ACTIVE=1, MUTED=2
  int last_microphone_capture_state_ = 0;  // WebKitMediaCaptureState: NONE=0, ACTIVE=1, MUTED=2

  // Fullscreen state (for DOM fullscreen requests)
  bool is_fullscreen_ = false;
  bool waiting_fullscreen_notify_ = false;

  // Activity/focus state
  // is_focused_ 初始 false（对齐 WebKitGTK 实际状态）：WebKit 收到 focus-in
  // 前 ViewIsFocused 不成立；若初始为 true，首个 setFocused(true) 会被幂等
  // 保护吞掉，焦点链路永不启动（caret 不显示）。
  bool is_focused_ = false;
  bool is_visible_ = true;

  // Target refresh rate (0 = default)
  uint32_t target_refresh_rate_ = 0;

  // Download signal handler ID
  gulong download_started_handler_id_ = 0;

  // Context menu state
  std::unique_ptr<ContextMenuPopup> context_menu_popup_;
  WebKitContextMenu* pending_context_menu_ = nullptr;
  WebKitHitTestResult* pending_hit_test_result_ = nullptr;
  double context_menu_x_ = 0;  // Mouse position when context menu was requested
  double context_menu_y_ = 0;
  double texture_offset_x_ = 0;  // Texture offset within the Flutter window
  double texture_offset_y_ = 0;

  // Option menu state (for HTML <select> dropdowns)
  std::unique_ptr<OptionMenuPopup> option_menu_popup_;

  // Pointer lock handler
  std::function<bool(bool)> pointer_lock_handler_;
  bool pointer_locked_ = false;

  // === Initialization ===
  void InitWebView(const InAppWebViewCreationParams& params);
  void RegisterEventHandlers();
  void PrepareAndAddUserScripts();  // Add plugin scripts based on settings

  // === WebKitGTK backend methods (implemented in in_app_webview_gtk.cc) ===
  // Creates the offscreen host window, mounts the widget and realizes it.
  // 创建屏内原点 popup 宿主（手动 redirect 隐形 + input shape 输入穿透）
  // 并启动 XComposite 捕获（GPU 直通唯一管线）。
  void InitGtkHost();
  // Destroys the offscreen host window (must run before webview_ is unref'ed).
  void ShutdownGtkHost();
  // 宿主 X 窗口 XShape bounding/input 全空：屏内定位后唯一隐形保证。
  // X server 端强制 clip，直接输出（无合成器）与合成器绘制（muffin 画 OR
  // 窗口）两条路径均不产生像素；geometry 不变，screenX 语义不受影响。
  // 扩展缺失显式报错不回退。
  void HideHostWindow(GdkWindow* host_gdk);
  // GPU 直通强制补帧入口（resize/scale 变化后重取当前内容别名并入队）。
  // 方法名保留以复用历史调用点。
  void RequestSnapshot();
  // Synthesizes a GdkEvent targeted at the webview widget and dispatches it.
  void DispatchGdkEvent(GdkEvent* event);

  // === Input synthesis (implemented in in_app_webview_gtk.cc) ===
  void GtkSetCursorPos(double x, double y);
  void GtkSetPointerButton(int kind, int button, int clickCount);
  void GtkSetScrollDelta(double dx, double dy, bool precise);
  void GtkSendKeyEvent(int type, int64_t keyCode, int scanCode, uint32_t modifiers);
  void GtkSetFocused(bool focused);
  void GtkSendTouchEvent(int type, int id, double x, double y,
                         const std::vector<std::tuple<int, double, double, int>>& touchPoints);

 public:
  // DOM fullscreen request handler (called from WebKit signal)
  bool OnDomFullscreenRequest(bool fullscreen);

  // Color picker state (for <input type="color"> support in WPE)
  // Public because accessed from C-style GTK callback
  std::string pending_color_input_value_;     // Current color from the input
  GtkWidget* active_color_dialog_ = nullptr;  // Active color picker dialog (non-blocking)
  bool active_color_alpha_enabled_ = false;   // Alpha enabled for active dialog
  int64_t color_dialog_show_time_ = 0;  // Time when dialog was shown (to prevent immediate close)
  WebKitScriptMessageReply* pending_color_reply_ = nullptr;  // WebKit reply for Promise resolution

  // Date picker state (for <input type="date/time/etc.> support in WPE)
  // Public because accessed from C-style GTK callback
  std::string pending_date_input_value_;     // Current value from the input
  std::string pending_date_input_type_;      // Type: date, datetime-local, time, month, week
  std::string pending_date_input_min_;       // Min constraint
  std::string pending_date_input_max_;       // Max constraint
  GtkWidget* active_date_dialog_ = nullptr;  // Active date picker dialog
  int64_t date_dialog_show_time_ = 0;        // Time when dialog was shown
  WebKitScriptMessageReply* pending_date_reply_ = nullptr;  // WebKit reply for Promise resolution

  // File chooser state (for <input type="file"> support)
  // Public because accessed from C-style GTK callback
  GtkWidget* active_file_dialog_ = nullptr;  // Active file chooser dialog (non-blocking)
  int64_t file_dialog_show_time_ = 0;     // Time when dialog was shown (to prevent immediate close)
  void* file_chooser_context_ = nullptr;  // Opaque pointer to FileChooserContext (for cleanup)

  // Option menu state (for HTML <select> support)
  WebKitOptionMenu* webkit_option_menu_ =
      nullptr;  // WebKit's option menu object (kept alive during popup)

  // Pointer lock handler (called from upper-layer negotiation)
  bool OnPointerLockRequest(bool lock);

 private:
  // === WebKit signals (same as WebKitGTK) ===
  // 延迟映射宿主（首帧同步等帧修复）：load-changed FINISHED/FAILED 或兜底
  // 定时器调用；幂等。gtk_host_window_ 为空（普通嵌窗模式）时 no-op。
  void MapHostNow();

  static void OnLoadChanged(WebKitWebView* web_view, WebKitLoadEvent load_event,
                            gpointer user_data);

  static gboolean OnDecidePolicy(WebKitWebView* web_view, WebKitPolicyDecision* decision,
                                 WebKitPolicyDecisionType decision_type, gpointer user_data);

  static void OnNotifyEstimatedLoadProgress(GObject* object, GParamSpec* pspec, gpointer user_data);

  static void OnNotifyTitle(GObject* object, GParamSpec* pspec, gpointer user_data);

  static void OnNotifyUri(GObject* object, GParamSpec* pspec, gpointer user_data);

  static gboolean OnLoadFailed(WebKitWebView* web_view, WebKitLoadEvent load_event,
                               gchar* failing_uri, GError* error, gpointer user_data);

  static gboolean OnLoadFailedWithTlsErrors(WebKitWebView* web_view, gchar* failing_uri,
                                            GTlsCertificate* certificate,
                                            GTlsCertificateFlags errors, gpointer user_data);

  static void OnCloseRequest(WebKitWebView* web_view, gpointer user_data);

  static WebKitWebView* OnCreateWebView(WebKitWebView* web_view,
                                        WebKitNavigationAction* navigation_action,
                                        gpointer user_data);

  static gboolean OnScriptDialog(WebKitWebView* web_view, WebKitScriptDialog* dialog,
                                 gpointer user_data);

  static gboolean OnPermissionRequest(WebKitWebView* web_view, WebKitPermissionRequest* request,
                                      gpointer user_data);

  static gboolean OnAuthenticate(WebKitWebView* web_view, WebKitAuthenticationRequest* request,
                                 gpointer user_data);

  // WebKitGTK 4.1（2.40 起）的 context-menu 信号在 context_menu 与
  // hit_test_result 之间插入了 GdkEvent* 参数（GDK_TYPE_EVENT |
  // G_SIGNAL_TYPE_STATIC_SCOPE）。若按旧 4 参签名接信号，emit 时会把
  // hit_test_result 传进 user_data 槽，导致 self 悬空、右键即 segfault。
  static gboolean OnContextMenu(WebKitWebView* web_view, WebKitContextMenu* context_menu,
                                GdkEvent* event, WebKitHitTestResult* hit_test_result,
                                gpointer user_data);

  static void OnContextMenuDismissed(WebKitWebView* web_view, gpointer user_data);

  static gboolean OnEnterFullscreen(WebKitWebView* web_view, gpointer user_data);

  static gboolean OnLeaveFullscreen(WebKitWebView* web_view, gpointer user_data);

  static void OnMouseTargetChanged(WebKitWebView* web_view, WebKitHitTestResult* hit_test_result,
                                   guint modifiers, gpointer user_data);

  static void OnWebProcessTerminated(WebKitWebView* web_view,
                                     WebKitWebProcessTerminationReason reason, gpointer user_data);

  static gboolean OnRunFileChooser(WebKitWebView* web_view, WebKitFileChooserRequest* request,
                                   gpointer user_data);

  // 同 context-menu：GTK 4.1 的 show-option-menu 信号为
  // (menu, GdkEvent*, rectangle)，旧 4 参签名会把 rectangle 静态指针读进
  // user_data 槽，<select> 下拉弹出即 segfault。
  static gboolean OnShowOptionMenu(WebKitWebView* web_view, WebKitOptionMenu* menu, GdkEvent* event,
                                   WebKitRectangle* rectangle, gpointer user_data);

  // === Download Signals ===
  static void OnDownloadStarted(WebKitNetworkSession* network_session, WebKitDownload* download,
                                gpointer user_data);

  // === Navigation State Signals ===
  static void OnBackForwardListChanged(WebKitBackForwardList* list,
                                       WebKitBackForwardListItem* item_added,
                                       gpointer items_removed, gpointer user_data);

  // === Media Capture State Signals ===
  static void OnNotifyCameraCaptureState(GObject* object, GParamSpec* pspec, gpointer user_data);
  static void OnNotifyMicrophoneCaptureState(GObject* object, GParamSpec* pspec,
                                             gpointer user_data);

  // === JavaScript bridge ===
  void dispatchPlatformReady();

  // === Custom Scheme Handler ===
  void RegisterCustomSchemes();
  static void OnCustomSchemeRequest(WebKitURISchemeRequest* request, gpointer user_data);

  // === Cursor detection ===
  void updateCursorFromCssStyle(const std::string& cursor_style);
};

}  // namespace flutter_inappwebview_plugin

#endif  // FLUTTER_INAPPWEBVIEW_PLUGIN_IN_APP_WEBVIEW_H_
