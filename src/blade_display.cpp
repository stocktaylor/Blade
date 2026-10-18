// blade - the dashboard's console settings (XConfig), kept by Blade, and the
// Display settings mapped onto how launched games run.
//
// The dashboard saves its System settings with ExSetXConfigSetting and reads
// them back with ExGetXConfigSetting. In the SDK the setter is a stub and the
// getter returns fixed values, so a choice like Display > Screen Format never
// stuck. Blade keeps the values the dashboard writes in xconfig.toml (next to
// launcher.toml) and returns them on reads; anything it hasn't stored comes
// from the SDK's defaults.
//
// Display > Screen Resolution: XGetAVPack says VGA (6), so the dashboard
// offers its VGA list and saves the choice as XCONFIG_USER 0x15 (u16 width,
// u16 height); 0 and 3 (component) would use 0x14 (sub_921248C8). The list is
// a table of six 8-byte entries {s16 width, s16 height, u16, u16 flags} in the
// image. Blade rewrites it with window sizes (rexlauncher's presets plus 4K),
// and passes the saved size to launched games as --window_width/height.
// Display > Screen Format is XCONFIG_USER 0x0A (video flags), widescreen =
// 0x00010000. strings.toml relabels it Windowed / Fullscreen and Blade passes
// it as --fullscreen; it also still sets the dashboard's own 4:3 / 16:9 layout.
//
// The stock build (BLADE_LAUNCHER=OFF) only keeps the XConfig values; the
// resolution list, the window and launched games are left alone.

#include "blade_display.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

// The SDK's XConfig defaults (librexruntime exports it; no public header).
namespace rex::kernel::xboxkrnl {
X_STATUS xeExGetXConfigSetting(uint16_t category, uint16_t setting, void* buffer,
                               uint16_t buffer_size, uint16_t* required_size);
}

namespace blade {
namespace {

constexpr uint32_t kUserCategory = 3;
constexpr uint32_t kVideoFlags = kUserCategory << 16 | 0x000A;
constexpr uint32_t kVgaResolution = kUserCategory << 16 | 0x0015;
constexpr uint32_t kWidescreenFlag = 0x00010000;  // in kVideoFlags

struct Size {
  uint16_t width, height;
};
// Screen Resolution until the dashboard saves one; in both lists below.
constexpr Size kDefaultSize = {1280, 720};
#if BLADE_LAUNCHER
// The dashboard's VGA list, as found in the image, and what replaces it:
// rexlauncher's window presets plus 4K. Same count, so the table keeps its size.
constexpr std::array<Size, 6> kDashSizes = {
    {{640, 480}, {848, 480}, {1024, 768}, {1280, 720}, {1280, 768}, {1360, 768}}};
constexpr std::array<Size, 6> kWindowSizes = {
    {{1280, 720}, {1280, 800}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}}};
#endif

std::mutex g_mutex;
std::filesystem::path g_path;
std::map<uint32_t, std::vector<uint8_t>> g_values;  // (category << 16 | setting) -> bytes
std::set<uint32_t> g_logged_gets;  // reads are frequent: log each setting once
#if BLADE_LAUNCHER
std::function<void(const DisplayChoice&)> g_on_display_changed;
#endif

std::string Key(uint32_t id) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04X.%04X", id >> 16, id & 0xFFFF);
  return buf;
}

std::string Hex(const uint8_t* p, size_t n) {
  std::string s;
  char buf[4];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(buf, sizeof(buf), "%02X", p[i]);
    s += buf;
  }
  return s;
}

std::vector<uint8_t> Unhex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) {
    out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  }
  return out;
}

uint16_t BE16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

void Save() {
  if (g_path.empty()) return;
  std::ofstream out(g_path, std::ios::binary);
  out << "# Console settings the Blade dashboard saved (ExSetXConfigSetting),\n"
         "# \"category.setting\" = guest bytes in hex. Written by Blade.\n\n[xconfig]\n";
  for (const auto& [id, bytes] : g_values) {
    out << '"' << Key(id) << "\" = \"" << Hex(bytes.data(), bytes.size()) << "\"\n";
  }
}

// Values Blade answers for settings the SDK doesn't know.
bool DefaultValue(uint32_t id, std::vector<uint8_t>* out) {
  if (id == kVgaResolution) {
    const Size& s = kDefaultSize;
    *out = {uint8_t(s.width >> 8), uint8_t(s.width), uint8_t(s.height >> 8), uint8_t(s.height)};
    return true;
  }
  return false;
}

}  // namespace

void LoadDisplaySettings(const std::filesystem::path& config_dir) {
  std::lock_guard lock(g_mutex);
  g_path = config_dir / "xconfig.toml";
  if (!std::filesystem::exists(g_path)) return;
  try {
    auto root = toml::parse_file(g_path.string());
    if (auto* t = root["xconfig"].as_table()) {
      for (auto& [k, v] : *t) {
        unsigned cat = 0, set = 0;
        auto s = v.value<std::string>();
        if (!s || std::sscanf(std::string(k.str()).c_str(), "%X.%X", &cat, &set) != 2) continue;
        g_values[cat << 16 | set] = Unhex(*s);
      }
    }
    REXLOG_INFO("XConfig: {} saved setting(s) from {}", g_values.size(), g_path.string());
  } catch (const toml::parse_error& e) {
    REXLOG_ERROR("XConfig: {}: {}", g_path.string(), e.what());
  }
}

#if BLADE_LAUNCHER

void PatchResolutionList() {
  auto module = REX_KERNEL_STATE()->GetExecutableModule();
  if (!module || !module->xex_module()) return;
  uint32_t base = module->xex_module()->base_address();
  uint32_t size = module->xex_module()->image_size();
  auto* memory = rex::system::kernel_memory();
  auto* image = memory->TranslateVirtual<uint8_t*>(base);

  constexpr uint32_t kStride = 8;
  constexpr uint32_t kTableSize = kStride * kDashSizes.size();
  std::vector<uint32_t> found;
  for (uint32_t off = 0; off + kTableSize <= size; off += 2) {
    bool match = true;
    for (size_t i = 0; i < kDashSizes.size() && match; ++i) {
      const uint8_t* e = image + off + i * kStride;
      match = BE16(e) == kDashSizes[i].width && BE16(e + 2) == kDashSizes[i].height;
    }
    if (match) found.push_back(base + off);
  }
  if (found.size() != 1) {
    REXLOG_WARN("Display: found {} copies of the VGA resolution table; list left as is",
                found.size());
    return;
  }

  uint32_t table = found[0];
  uint8_t* t = memory->TranslateVirtual<uint8_t*>(table);
  REXLOG_INFO("Display: VGA resolution table at {:08X}: {}", table, Hex(t, kTableSize));
  std::array<uint8_t, kTableSize> original;
  std::memcpy(original.data(), t, kTableSize);

  auto* heap = memory->LookupHeap(table);
  uint32_t old_protect = 0;
  bool reprotect =
      heap && heap->Protect(table, kTableSize,
                            rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite,
                            &old_protect);
  for (size_t i = 0; i < kWindowSizes.size(); ++i) {
    // Keep the other four bytes of the original entry nearest in aspect ratio.
    double aspect = double(kWindowSizes[i].width) / kWindowSizes[i].height;
    size_t nearest = 0;
    for (size_t j = 1; j < kDashSizes.size(); ++j) {
      auto diff = [&](size_t k) {
        return std::abs(double(kDashSizes[k].width) / kDashSizes[k].height - aspect);
      };
      if (diff(j) < diff(nearest)) nearest = j;
    }
    uint8_t* e = t + i * kStride;
    std::memcpy(e, original.data() + nearest * kStride, kStride);
    // Flags bit 0x8000 marks a widescreen VGA mode: Screen Format then shows
    // Widescreen and ignores the saved choice (sub_9211C1F8). Screen Format is
    // Blade's windowed/fullscreen switch, so keep it selectable.
    e[6] &= 0x7F;
    e[0] = uint8_t(kWindowSizes[i].width >> 8);
    e[1] = uint8_t(kWindowSizes[i].width);
    e[2] = uint8_t(kWindowSizes[i].height >> 8);
    e[3] = uint8_t(kWindowSizes[i].height);
  }
  if (reprotect) heap->Protect(table, kTableSize, old_protect);
  REXLOG_INFO("Display: resolution list now {}", Hex(t, kTableSize));
}

DisplayChoice CurrentDisplayChoice() {
  std::lock_guard lock(g_mutex);
  DisplayChoice c;
  if (auto it = g_values.find(kVgaResolution); it != g_values.end() && it->second.size() >= 4) {
    c.width = BE16(it->second.data());
    c.height = BE16(it->second.data() + 2);
  }
  if (auto it = g_values.find(kVideoFlags); it != g_values.end() && it->second.size() >= 4) {
    uint32_t flags = uint32_t(it->second[0]) << 24 | uint32_t(it->second[1]) << 16 |
                     uint32_t(it->second[2]) << 8 | it->second[3];
    c.fullscreen = (flags & kWidescreenFlag) != 0;
  }
  return c;
}

void SetDisplayChangedCallback(std::function<void(const DisplayChoice&)> callback) {
  std::lock_guard lock(g_mutex);
  g_on_display_changed = std::move(callback);
}

std::vector<std::string> DisplayLaunchArgs() {
  DisplayChoice c = CurrentDisplayChoice();
  std::vector<std::string> args;
  if (c.width && c.height) {
    args.push_back("--window_width=" + std::to_string(c.width));
    args.push_back("--window_height=" + std::to_string(c.height));
  }
  if (c.fullscreen) {
    args.push_back(std::string("--fullscreen=") + (*c.fullscreen ? "true" : "false"));
  }
  return args;
}

#endif  // BLADE_LAUNCHER

}  // namespace blade

//=============================================================================
// Import overrides (same symbols as the SDK's; the executable's win)
//=============================================================================

namespace {
using namespace rex;

u32 BladeExGetXConfigSetting(u16 category, u16 setting, mapped_void buffer, u16 buffer_size,
                             mapped_u16 required_size) {
  uint32_t id = uint32_t(category) << 16 | setting;
  {
    std::lock_guard lock(blade::g_mutex);
    std::vector<uint8_t> fallback;
    const std::vector<uint8_t>* v = nullptr;
    const char* source = "saved";
    if (auto it = blade::g_values.find(id); it != blade::g_values.end()) {
      v = &it->second;
    } else if (blade::DefaultValue(id, &fallback)) {
      v = &fallback;
      source = "Blade default";
    }
    if (v) {
      if (required_size) *required_size = uint16_t(v->size());
      if (buffer) {
        if (buffer_size < v->size()) return X_STATUS_BUFFER_TOO_SMALL;
        std::memcpy(buffer.host_address(), v->data(), v->size());
      }
      if (blade::g_logged_gets.insert(id).second) {
        REXLOG_INFO("XConfig: get {} -> {} ({})", blade::Key(id),
                    blade::Hex(v->data(), v->size()), source);
      }
      return X_STATUS_SUCCESS;
    }
  }
  uint16_t size = 0;
  X_STATUS result = rex::kernel::xboxkrnl::xeExGetXConfigSetting(
      category, setting, buffer ? buffer.host_address() : nullptr, buffer_size, &size);
  if (required_size) *required_size = size;
  bool first;
  {
    std::lock_guard lock(blade::g_mutex);
    first = blade::g_logged_gets.insert(id).second;
  }
  if (first) {
    REXLOG_INFO("XConfig: get {} -> {:08X} {}", blade::Key(id), result,
                (XSUCCEEDED(result) && buffer)
                    ? blade::Hex(static_cast<const uint8_t*>(buffer.host_address()), size)
                    : std::string());
  }
  return result;
}

u32 BladeExSetXConfigSetting(u16 category, u16 setting, mapped_void buffer, u16 buffer_size) {
  uint32_t id = uint32_t(category) << 16 | setting;
  if (!buffer && buffer_size) return X_STATUS_INVALID_PARAMETER;
  std::vector<uint8_t> bytes;
  if (buffer) {
    auto* p = static_cast<const uint8_t*>(buffer.host_address());
    bytes.assign(p, p + buffer_size);
  }
  REXLOG_INFO("XConfig: set {} = {}", blade::Key(id), blade::Hex(bytes.data(), bytes.size()));
#if BLADE_LAUNCHER
  std::function<void(const blade::DisplayChoice&)> on_changed;
#endif
  {
    std::lock_guard lock(blade::g_mutex);
    blade::g_values[id] = std::move(bytes);
    blade::g_logged_gets.erase(id);  // log the next read of the new value
    blade::Save();
#if BLADE_LAUNCHER
    if (id == blade::kVgaResolution || id == blade::kVideoFlags) {
      on_changed = blade::g_on_display_changed;
    }
#endif
  }
#if BLADE_LAUNCHER
  if (on_changed) on_changed(blade::CurrentDisplayChoice());
#endif
  return X_STATUS_SUCCESS;
}

}  // namespace

REX_HOOK(__imp__ExGetXConfigSetting, BladeExGetXConfigSetting)
REX_HOOK(__imp__ExSetXConfigSetting, BladeExSetXConfigSetting)

// Picking a resolution calls this with the new mode (sub_9211BC40) before the
// "can you see a picture?" prompt; logged to see that flow.
REX_HOOK_RAW(__imp__VdSetDisplayModeOverride) {
  (void)base;
  REXLOG_INFO("VdSetDisplayModeOverride(r3={:08X}, r4={:08X}, f1={}, r6={:08X}, r7={:08X})",
              ctx.r3.u32, ctx.r4.u32, ctx.f1.f64, ctx.r6.u32, ctx.r7.u32);
  ctx.r3.u64 = 0;
}
