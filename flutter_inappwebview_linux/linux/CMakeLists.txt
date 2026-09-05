# The Flutter tooling requires that developers have CMake 3.10 or later
# installed. You should not increase this version, as doing so will cause
# the plugin to fail to compile for some customers of the plugin.
cmake_minimum_required(VERSION 3.10)

# Project-level configuration.
set(PROJECT_NAME "flutter_inappwebview_linux")
project(${PROJECT_NAME} LANGUAGES CXX C)

# This value is used when generating builds using this plugin, so it must
# not be changed.
set(PLUGIN_NAME "flutter_inappwebview_linux_plugin")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# nlohmann/json version for JSON parsing
set(NLOHMANN_JSON_VERSION "3.11.3")

# === Backend ===
# 唯一后端：WebKitGTK（webkit2gtk-4.1，Tauri 2 同款，发行版官方包）。
# 渲染采用 GPU 直通唯一管线（XComposite + Damage + EGLImage 零拷贝，
# 见 in_app_webview/webkit_gpu_capture.*），无 CPU 软渲染回退。

find_package(PkgConfig REQUIRED)

# Always require epoxy for OpenGL/EGL support
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)

# GTK is needed for GDK (display access, event handling, input synthesis)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)

# === WebKitGTK (webkit2gtk-4.1, libsoup3) ===
# Present in Ubuntu 22.04+, Debian 12+, Fedora, Arch official repositories.
pkg_check_modules(WEBKIT_GTK REQUIRED IMPORTED_TARGET webkit2gtk-4.1)
if(WEBKIT_GTK_VERSION VERSION_LESS "2.40")
  message(WARNING "flutter_inappwebview_linux: WebKitGTK ${WEBKIT_GTK_VERSION} is older than\n"
                  "recommended 2.40. Some features may be degraded.")
endif()
message(STATUS "flutter_inappwebview_linux: Found webkit2gtk-4.1 (${WEBKIT_GTK_VERSION})")

# Check for libsecret for secure credential storage
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
message(STATUS "flutter_inappwebview_linux: Found libsecret-1 (${LIBSECRET_VERSION})")

# === nlohmann/json dependency for JSON parsing ===
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v${NLOHMANN_JSON_VERSION}/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Don't build tests/examples
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)
# Suppress deprecated literal operator warnings from nlohmann_json
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-literal-operator")
message(STATUS "flutter_inappwebview_linux: nlohmann/json ${NLOHMANN_JSON_VERSION} configured")

# Plugin source files
list(APPEND PLUGIN_SOURCES
  "flutter_inappwebview_linux_plugin.cc"
  "plugin_instance.cc"
  "cookie_manager.cc"
  "credential_database.cc"
  "proxy_manager.cc"
  "web_storage_manager.cc"
  "webview_environment.cc"
  "content_blocker/content_blocker_handler.cc"
  "find_interaction/find_interaction_controller.cc"
  "find_interaction/find_interaction_channel_delegate.cc"
  "headless_in_app_webview/headless_in_app_webview.cc"
  "headless_in_app_webview/headless_in_app_webview_manager.cc"
  "headless_in_app_webview/headless_webview_channel_delegate.cc"
  "in_app_browser/in_app_browser.cc"
  "in_app_browser/in_app_browser_channel_delegate.cc"
  "in_app_browser/in_app_browser_manager.cc"
  "in_app_browser/in_app_browser_settings.cc"
  "in_app_webview/in_app_webview_manager.cc"
  "in_app_webview/custom_platform_view.cc"
  "in_app_webview/in_app_webview.cc"
  "in_app_webview/in_app_webview_gtk.cc"
  "in_app_webview/im_fix.cc"
  "in_app_webview/in_app_webview_settings.cc"
  "in_app_webview/user_content_controller.cc"
  "in_app_webview/webview_channel_delegate.cc"
  "in_app_webview/webkit_gpu_capture.cc"
  "in_app_webview/inappwebview_gpu_texture.cc"
  "types/channel_delegate.cc"
  "types/client_cert_challenge.cc"
  "types/client_cert_response.cc"
  "types/content_world.cc"
  "types/context_menu_popup.cc"
  "types/create_window_action.cc"
  "types/custom_scheme_response.cc"
  "types/download_start_request.cc"
  "types/download_start_response.cc"
  "types/find_session.cc"
  "types/http_auth_response.cc"
  "types/http_authentication_challenge.cc"
  "types/javascript_handler_function_data.cc"
  "types/js_alert_request.cc"
  "types/js_alert_response.cc"
  "types/js_before_unload_response.cc"
  "types/js_confirm_request.cc"
  "types/js_confirm_response.cc"
  "types/js_prompt_request.cc"
  "types/js_prompt_response.cc"
  "types/hit_test_result.cc"
  "types/navigation_action.cc"
  "types/option_menu_popup.cc"
  "types/permission_request.cc"
  "types/permission_response.cc"
  "types/plugin_script.cc"
  "types/server_trust_auth_response.cc"
  "types/server_trust_challenge.cc"
  "types/show_file_chooser_response.cc"
  "types/ssl_certificate.cc"
  "types/url_credential.cc"
  "types/url_protection_space.cc"
  "types/url_request.cc"
  "types/user_script.cc"
  "types/web_resource_error.cc"
  "types/web_resource_request.cc"
  "types/web_resource_response.cc"
  "types/web_view_transport.cc"
  "web_message/web_message_channel.cc"
  "web_message/web_message_listener.cc"
  "web_message/web_message_listener_channel_delegate.cc"
)

# Define the plugin library target. Its name must not be changed (see comment
# on PLUGIN_NAME above).
add_library(${PLUGIN_NAME} SHARED
  ${PLUGIN_SOURCES}
)

# Apply a standard set of build settings that are configured in the
# application-level CMakeLists.txt. This can be removed for plugins that want
# full control over build settings.
apply_standard_settings(${PLUGIN_NAME})

# Symbols are hidden by default to reduce the chance of accidental conflicts
# between plugins. This should not be removed; any symbols that should be
# exported should be explicitly exported with the FLUTTER_PLUGIN_EXPORT macro.
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

set_target_properties(${PLUGIN_NAME} PROPERTIES
  INSTALL_RPATH "$ORIGIN"
  BUILD_RPATH "$ORIGIN"
  BUILD_WITH_INSTALL_RPATH TRUE)

# Source include directories and library dependencies
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  ${EPOXY_INCLUDE_DIRS}
  ${WEBKIT_GTK_INCLUDE_DIRS})

target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::EPOXY)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::WEBKIT_GTK)
target_link_libraries(${PLUGIN_NAME} PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::LIBSECRET)

# GPU 直通捕获依赖：X11 窗口语义 + XComposite redirect + XDamage 帧驱动
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XCOMPOSITE REQUIRED IMPORTED_TARGET xcomposite)
pkg_check_modules(XDAMAGE REQUIRED IMPORTED_TARGET xdamage)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::X11)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::XCOMPOSITE)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::XDAMAGE)

# === Library Bundling ===
# WebKitGTK 经由系统包提供，无需打包额外库。
set(flutter_inappwebview_linux_bundled_libraries
  ""
  PARENT_SCOPE
)

# === Tests ===
# These unit tests can be run from a terminal after building the example.

# Only enable test builds when building the example (which sets this variable)
# so that plugin clients aren't building the tests.
if (${include_${PROJECT_NAME}_tests})
if(${CMAKE_VERSION} VERSION_LESS "3.11.0")
message("Unit tests require CMake 3.11.0 or later")
else()
set(TEST_RUNNER "${PROJECT_NAME}_test")
enable_testing()

# Add the Google Test dependency.
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/release-1.11.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
# Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Disable install commands for gtest so it doesn't end up in the bundle.
set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)

FetchContent_MakeAvailable(googletest)

# The plugin's exported API is not very useful for unit testing, so build the
# sources directly into the test binary rather than using the shared library.
add_executable(${TEST_RUNNER}
  test/flutter_inappwebview_linux_plugin_test.cc
  ${PLUGIN_SOURCES}
)
apply_standard_settings(${TEST_RUNNER})
target_include_directories(${TEST_RUNNER} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  ${EPOXY_INCLUDE_DIRS}
  ${WEBKIT_GTK_INCLUDE_DIRS})
target_link_libraries(${TEST_RUNNER} PRIVATE flutter)
target_link_libraries(${TEST_RUNNER} PRIVATE PkgConfig::GTK)
target_link_libraries(${TEST_RUNNER} PRIVATE PkgConfig::EPOXY)
target_link_libraries(${TEST_RUNNER} PRIVATE PkgConfig::WEBKIT_GTK)
target_link_libraries(${TEST_RUNNER} PRIVATE PkgConfig::X11)
target_link_libraries(${TEST_RUNNER} PRIVATE PkgConfig::XCOMPOSITE)
target_link_libraries(${TEST_RUNNER} PRIVATE PkgConfig::XDAMAGE)
target_link_libraries(${TEST_RUNNER} PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(${TEST_RUNNER} PRIVATE PkgConfig::LIBSECRET)
target_link_libraries(${TEST_RUNNER} PRIVATE gtest_main gmock)

# Enable automatic test discovery.
include(GoogleTest)
gtest_discover_tests(${TEST_RUNNER})

endif()  # CMake version check
endif()  # include_${PROJECT_NAME}_tests
