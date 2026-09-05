#include "im_fix.h"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace {

struct ImFixEntry {
  void* owner;
  Window xid;
  int dx;
  int dy;
};

std::mutex g_mutex;
std::vector<ImFixEntry> g_entries;
std::atomic<int> g_entry_count{0};
std::atomic<bool> g_debug{false};

using XTranslateCoordinatesFn = Status (*)(Display*, Window, Window, int, int,
                                           int*, int*, Window*);

inline bool IsRootWindow(Display* dpy, Window w) {
  // DefaultScreenOfDisplay/RootWindowOfScreen 是纯结构体访问，无 X 往返
  return w == RootWindowOfScreen(DefaultScreenOfDisplay(dpy));
}

}  // namespace

extern "C" {

void vai_imfix_register(void* owner, Window xid, int dx, int dy) {
  if (owner == nullptr || xid == None) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  bool changed = false;
  for (auto& e : g_entries) {
    if (e.owner == owner && e.xid == xid) {
      if (e.dx != dx || e.dy != dy) {
        e.dx = dx;
        e.dy = dy;
        changed = true;
      }
      g_entry_count.store(static_cast<int>(g_entries.size()),
                          std::memory_order_release);
      if (changed && g_debug.load(std::memory_order_relaxed)) {
        fprintf(stderr,
                "[vai-imfix] update owner=%p xid=0x%lx delta=(%d,%d)\n", owner,
                (unsigned long)xid, dx, dy);
      }
      return;
    }
  }
  g_entries.push_back(ImFixEntry{owner, xid, dx, dy});
  g_entry_count.store(static_cast<int>(g_entries.size()),
                      std::memory_order_release);
  if (g_debug.load(std::memory_order_relaxed)) {
    fprintf(stderr, "[vai-imfix] register owner=%p xid=0x%lx delta=(%d,%d)\n",
            owner, (unsigned long)xid, dx, dy);
  }
}

void vai_imfix_unregister_owner(void* owner) {
  if (owner == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  size_t before = g_entries.size();
  g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
                                 [owner](const ImFixEntry& e) {
                                   return e.owner == owner;
                                 }),
                  g_entries.end());
  if (g_entries.size() != before) {
    g_entry_count.store(static_cast<int>(g_entries.size()),
                        std::memory_order_release);
    if (g_debug.load(std::memory_order_relaxed)) {
      fprintf(stderr, "[vai-imfix] unregister owner=%p (-%zu)\n", owner,
              before - g_entries.size());
    }
  }
}

void vai_imfix_set_debug(int enable) {
  g_debug.store(enable != 0, std::memory_order_relaxed);
}

// 启动时读环境变量开关：VAI_IMFIX_DEBUG=1 后 hook 命中/注册变化打印 stderr。
// （高频路径不依赖进程外主动调 set_debug）
__attribute__((constructor)) static void vai_imfix_read_env() {
  const char* env = getenv("VAI_IMFIX_DEBUG");
  if (env != nullptr && env[0] != '\0' && env[0] != '0') {
    g_debug.store(true, std::memory_order_relaxed);
  }
}

// 符号覆盖：本 so 默认 hidden（CMake CXX_VISIBILITY_PRESET hidden），hook 必须
// 显式 default 导出进入 .dynsym——fcitx im module（dlopen, RTLD_LOCAL）调用
// libgdk/libgtk/libX11 时从全局作用域（exe + 其 DT_NEEDED，含本 plugin so）
// 先解析到这里（实测：dlopen 的 GTK im module 场景同样命中）。
// RTLD_NEXT 取 libX11 真实现。
__attribute__((visibility("default"))) Status XTranslateCoordinates(
    Display* dpy, Window src_w, Window dest_w, int src_x, int src_y,
    int* dest_x, int* dest_y, Window* child_return) {
  static XTranslateCoordinatesFn real_fn = []() {
    void* fn = dlsym(RTLD_NEXT, "XTranslateCoordinates");
    if (fn == nullptr) {
      // libX11 必然存在（hook 文件本身链接了 X11），缺符号是致命装配错误
      fprintf(stderr, "[vai-imfix] FATAL: XTranslateCoordinates not found\n");
      abort();
    }
    return reinterpret_cast<XTranslateCoordinatesFn>(fn);
  }();

  Status status =
      real_fn(dpy, src_w, dest_w, src_x, src_y, dest_x, dest_y, child_return);

  // 快路径：注册表空（绝大多数应用/时刻）一次原子读直通
  if (g_entry_count.load(std::memory_order_acquire) == 0) {
    return status;
  }
  // 仅补偿"宿主树内窗口 → 根窗口"换算：fcitx5 候选框定位、菜单弹出等
  // 依赖该语义；相对窗口间换算（dest 非根）不补偿，避免双重位移。
  if (dpy == nullptr || !IsRootWindow(dpy, dest_w)) {
    return status;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& e : g_entries) {
    if (e.xid == src_w) {
      if (g_debug.load(std::memory_order_relaxed)) {
        fprintf(stderr,
                "[vai-imfix] translate xid=0x%lx (%d,%d) -> (%d,%d) "
                "delta=(%d,%d)\n",
                (unsigned long)src_w, *dest_x, *dest_y, *dest_x + e.dx,
                *dest_y + e.dy, e.dx, e.dy);
      }
      *dest_x += e.dx;
      *dest_y += e.dy;
      break;
    }
  }
  return status;
}

}  // extern "C"
