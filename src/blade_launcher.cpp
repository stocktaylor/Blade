// blade - the dashboard as a launcher for other rexglue recomps.
//
// Listing: Games > Demos fills its list from XamContentAggregateCreateEnumerator
// (all devices, content type 0x00080000, game demos). The SDK's version only
// lists the running title's own content, so the dashboard always saw nothing;
// Blade answers it from launcher.toml instead. (Arcade, type 0x000D0000, runs
// each item through further checks and rejected these entries.)
//
// Launching: the dashboard hands the selected item back to
// XamContentLaunchImageInternal (stubbed in the SDK). The item's file name is
// the registry id. Each recomp is its own executable with its own guest memory
// at fixed addresses, so a game can't run inside the dashboard's process:
// Blade re-execs itself as a relay, which frees the dashboard's GPU, audio and
// window, runs the game, and when it exits (quit, window closed, crash) execs
// the dashboard again with its original arguments.
//
// These overrides use REX_HOOK, which defines the same __imp__ symbols as the
// SDK's REX_EXPORTs: the generated code and blade_init.cpp bind to the
// executable's definition, which takes precedence over librexruntime.so's.
//
// Windows has no exec: the dashboard starts the relay as a new process and
// then terminates itself; the relay starts the game, waits for it, and starts
// a new dashboard the same way.

#include "blade_launcher.h"

#include "blade_display.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xcontent.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>
#include <rex/types.h>

REXCVAR_DEFINE_STRING(blade_launcher_config, "", "Blade",
                      "Launcher registry (default: launcher.toml next to the executable, "
                      "else next to the game folder)");

namespace blade {
namespace {

constexpr const char* kRelayFlag = "--blade-relay";

std::vector<LauncherTitle> g_titles;

#ifdef _WIN32

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string Narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::vector<std::string> ReadSelfCmdline() {
  int argc = 0;
  wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> args;
  for (int i = 0; wargv && i < argc; ++i) args.push_back(Narrow(wargv[i]));
  LocalFree(wargv);
  return args;
}

std::filesystem::path SelfExe() { return rex::filesystem::GetExecutablePath(); }

// One argument quoted for CommandLineToArgvW / the CRT's argv parser.
void AppendQuoted(std::wstring& cmd, const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    cmd += arg;
    return;
  }
  cmd += L'"';
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < arg.size() && arg[i] == L'\\') ++i, ++slashes;
    if (i == arg.size()) {
      cmd.append(slashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      cmd.append(slashes * 2 + 1, L'\\');
    } else {
      cmd.append(slashes, L'\\');
    }
    cmd += arg[i];
  }
  cmd += L'"';
}

// Starts args[0] with args; no handles are inherited. Returns the process
// handle (caller closes it), or nullptr on failure.
HANDLE Spawn(const std::vector<std::string>& args, const std::string& cwd) {
  std::wstring cmd;
  for (const auto& a : args) {
    if (!cmd.empty()) cmd += L' ';
    AppendQuoted(cmd, Widen(a));
  }
  std::wstring exe = Widen(args[0]);
  std::wstring wcwd = Widen(cwd);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
    return nullptr;
  }
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

// Runs before main() (a CRT initializer), so a relay never brings up the SDK,
// the GPU or a window.
//   blade --blade-relay <cwd> <n> <game argv (n)...> <dashboard argv...>
__attribute__((constructor)) void BladeRelayMain() {
  auto argv = ReadSelfCmdline();
  int argc = int(argv.size());
  if (argc < 5 || argv[1] != kRelayFlag) {
    return;
  }
  int n = std::atoi(argv[3].c_str());
  if (n < 1 || 4 + n > argc) {
    std::fprintf(stderr, "blade relay: bad arguments\n");
    ExitProcess(2);
  }
  std::vector<std::string> game(argv.begin() + 4, argv.begin() + 4 + n);
  std::vector<std::string> dash(argv.begin() + 4 + n, argv.end());

  std::fprintf(stderr, "blade relay: starting %s\n", game[0].c_str());
  if (HANDLE h = Spawn(game, argv[2])) {
    WaitForSingleObject(h, INFINITE);
    DWORD status = 0;
    GetExitCodeProcess(h, &status);
    CloseHandle(h);
    std::fprintf(stderr, "blade relay: game exited (status %lu), returning to dashboard\n",
                 status);
  } else {
    std::fprintf(stderr, "blade relay: start game failed (error %lu)\n", GetLastError());
  }

  if (!dash.empty()) {
    if (HANDLE h = Spawn(dash, "")) {
      CloseHandle(h);
      ExitProcess(0);
    }
    std::fprintf(stderr, "blade relay: start dashboard failed (error %lu)\n", GetLastError());
  }
  ExitProcess(1);
}

#else

std::vector<std::string> ReadSelfCmdline() {
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::string> args;
  size_t start = 0;
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\0') {
      args.emplace_back(raw, start, i - start);
      start = i + 1;
    }
  }
  return args;
}

std::filesystem::path SelfExe() {
  std::error_code ec;
  return std::filesystem::read_symlink("/proc/self/exe", ec);
}

std::vector<char*> ToArgv(std::vector<std::string>& args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  return argv;
}

// Runs before main() (glibc passes argc/argv to ELF constructors), so a relay
// never brings up the SDK, the GPU or a window.
//   blade --blade-relay <cwd> <n> <game argv (n)...> <dashboard argv...>
__attribute__((constructor)) void BladeRelayMain(int argc, char** argv, char**) {
  if (argc < 5 || std::strcmp(argv[1], kRelayFlag) != 0) {
    return;
  }
  // The dashboard's descriptors (the 4.5GB guest memory file among them) are
  // still open after exec unless marked CLOEXEC; don't hand them to the game.
  if (close_range(3, ~0U, 0) != 0) {
    for (int fd = 3; fd < 1024; ++fd) close(fd);
  }

  const char* cwd = argv[2];
  int n = std::atoi(argv[3]);
  if (n < 1 || 4 + n > argc) {
    std::fprintf(stderr, "blade relay: bad arguments\n");
    _exit(2);
  }
  char** game = argv + 4;
  char** dash = argv + 4 + n;

  std::vector<char*> game_argv(game, game + n);
  game_argv.push_back(nullptr);

  std::fprintf(stderr, "blade relay: starting %s\n", game[0]);
  pid_t pid = fork();
  if (pid == 0) {
    if (chdir(cwd) != 0) std::perror("blade relay: chdir");
    execv(game_argv[0], game_argv.data());
    std::perror("blade relay: exec game");
    _exit(127);
  }
  if (pid > 0) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    std::fprintf(stderr, "blade relay: game exited (status %d), returning to dashboard\n", status);
  } else {
    std::perror("blade relay: fork");
  }

  if (dash < argv + argc) {
    execv(dash[0], dash);
    std::perror("blade relay: exec dashboard");
  }
  _exit(1);
}

#endif

}  // namespace

void LoadLauncherConfig(const std::filesystem::path& game_data_root) {
  std::vector<std::filesystem::path> candidates;
  std::string configured = REXCVAR_GET(blade_launcher_config);
  if (!configured.empty()) {
    candidates.emplace_back(configured);
  } else {
    candidates.push_back(rex::filesystem::GetExecutableFolder() / "launcher.toml");
    candidates.push_back(std::filesystem::absolute(game_data_root).parent_path() /
                         "launcher.toml");
  }

  g_titles.clear();
  for (const auto& path : candidates) {
    if (!std::filesystem::exists(path)) continue;
    toml::table root;
    try {
      root = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
      REXLOG_ERROR("Launcher: {}: {}", path.string(), e.what());
      return;
    }
    auto base = path.parent_path();
    auto resolve = [&](std::string_view p) {
      std::filesystem::path fp(p);
      return fp.is_absolute() ? fp : std::filesystem::weakly_canonical(base / fp);
    };
    if (auto* titles = root["title"].as_table()) {
      for (auto& [key, node] : *titles) {
        auto* t = node.as_table();
        if (!t) continue;
        LauncherTitle title;
        title.id = std::string(key.str());
        title.name = (*t)["name"].value_or(title.id);
        title.title_id = static_cast<uint32_t>((*t)["title_id"].value_or(int64_t(0)));
        title.exe = resolve((*t)["exe"].value_or(std::string()));
        if (auto* args = (*t)["args"].as_array()) {
          for (auto& a : *args) title.args.push_back(a.value_or(std::string()));
        }
        auto cwd = (*t)["cwd"].value<std::string>();
        // Relative paths in args resolve against the cwd, so default it to this
        // file's folder: then every relative path in the file means the same.
        title.cwd = cwd ? resolve(*cwd) : base;
        if (title.id.size() > 42) {
          REXLOG_WARN("Launcher: id '{}' is longer than 42 chars, skipped", title.id);
          continue;
        }
        if (title.exe.empty() || !std::filesystem::exists(title.exe)) {
          REXLOG_WARN("Launcher: '{}': exe not found: {}", title.id, title.exe.string());
        }
        REXLOG_INFO("Launcher: '{}' ({}) -> {}", title.id, title.name, title.exe.string());
        g_titles.push_back(std::move(title));
      }
    }
    REXLOG_INFO("Launcher: {} title(s) from {}", g_titles.size(), path.string());
    return;
  }
  REXLOG_INFO("Launcher: no launcher.toml found; Games > Demos will be empty");
}

const std::vector<LauncherTitle>& LauncherTitles() { return g_titles; }

const LauncherTitle* FindLauncherTitle(std::string_view id) {
  for (const auto& t : g_titles) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

void LaunchTitle(const LauncherTitle& title) {
  auto self = SelfExe();
  auto dash_args = ReadSelfCmdline();
  if (self.empty() || dash_args.empty()) {
    REXLOG_ERROR("Launcher: can't determine this executable's path/arguments");
    return;
  }
  dash_args[0] = self.string();

  std::vector<std::string> args{self.string(), kRelayFlag, title.cwd.string(),
                                std::to_string(1 + title.args.size()), title.exe.string()};
  args.insert(args.end(), title.args.begin(), title.args.end());
  // Display settings chosen in the dashboard come after the title's own args.
  auto display = DisplayLaunchArgs();
  args.insert(args.end(), display.begin(), display.end());
  args[3] = std::to_string(1 + title.args.size() + display.size());
  args.insert(args.end(), dash_args.begin(), dash_args.end());

  REXLOG_INFO("Launcher: launching '{}' ({})", title.id, title.exe.string());
  spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& l) { l->flush(); });

#ifdef _WIN32
  // The dashboard's process has to go away to free the GPU, audio and window.
  if (HANDLE h = Spawn(args, "")) {
    CloseHandle(h);
    TerminateProcess(GetCurrentProcess(), 0);
  }
  REXLOG_ERROR("Launcher: start relay failed (error {})", GetLastError());
#else
  auto argv = ToArgv(args);
  execv(argv[0], argv.data());
  REXLOG_ERROR("Launcher: exec relay failed: {}", std::strerror(errno));
#endif
}

}  // namespace blade

//=============================================================================
// XAM overrides
//=============================================================================

namespace {
using namespace rex;  // X_RESULT / X_HRESULT, which the X_E_* / X_ERROR_* macros name
using namespace rex::system;
using namespace rex::system::xam;

u32 BladeContentAggregateCreateEnumerator(u64 xuid, u32 device_id, u32 content_type, u32 unk3,
                                          mapped_u32 handle_out) {
  if (!handle_out) {
    return X_E_INVALIDARG;
  }
  auto e = make_object<XStaticEnumerator<XCONTENT_AGGREGATE_DATA>>(REX_KERNEL_STATE(), 1);
  X_KENUMERATOR_CONTENT_AGGREGATE* extra;
  auto result = e->Initialize(0xFF, 0xFE, 0x2000E, 0x20010, 0, &extra);
  if (XFAILED(result)) {
    return result;
  }
  extra->magic = kXObjSignature;
  extra->handle = e->handle();

  const uint32_t hdd = static_cast<uint32_t>(DummyDeviceId::HDD);
  if (XContentType(uint32_t(content_type)) == XContentType::kGameDemo &&
      (device_id == 0 || device_id == hdd)) {
    for (const auto& title : blade::LauncherTitles()) {
      auto* item = e->AppendItem();
      if (!item) break;
      std::memset(item, 0, sizeof(*item));
      item->device_id = hdd;
      item->content_type = XContentType::kGameDemo;
      item->set_display_name(rex::string::to_utf16(title.name));
      item->set_file_name(title.id);
      item->xuid = 0;
      item->title_id = title.title_id;
    }
  }
  REXLOG_INFO(
      "XamContentAggregateCreateEnumerator(xuid={:016X}, device={}, type={:08X}, unk={:X}) -> "
      "{} item(s)",
      uint64_t(xuid), uint32_t(device_id), uint32_t(content_type), uint32_t(unk3),
      e->item_count());

  *handle_out = e->handle();
  return X_ERROR_SUCCESS;
}

// Argument layout unknown (stubbed in Xenia too): log the raw registers and
// take the first one that points at an XCONTENT_DATA naming a launcher title.
const blade::LauncherTitle* FindTitleInArgs(PPCContext& ctx) {
  auto* memory = REX_KERNEL_MEMORY();
  const uint32_t regs[] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
  for (int i = 0; i < 4; ++i) {
    uint32_t addr = regs[i];
    if (addr < 0x10000) continue;
    auto* data = memory->TranslateVirtual<XCONTENT_DATA*>(addr);
    std::string file_name = data->file_name();
    REXLOG_INFO("  r{} = {:08X}: device={} type={:08X} file_name='{}'", 3 + i, addr,
                uint32_t(data->device_id),
                static_cast<uint32_t>(XContentType(data->content_type)), file_name);
    if (auto* title = blade::FindLauncherTitle(file_name)) {
      return title;
    }
  }
  return nullptr;
}

}  // namespace

REX_HOOK(__imp__XamContentAggregateCreateEnumerator, BladeContentAggregateCreateEnumerator)

REX_HOOK_RAW(__imp__XamContentLaunchImageInternal) {
  (void)base;
  REXLOG_INFO("XamContentLaunchImageInternal(r3={:08X}, r4={:08X}, r5={:08X}, r6={:08X})",
              ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  if (auto* title = FindTitleInArgs(ctx)) {
    blade::LaunchTitle(*title);  // returns only on failure
  } else {
    REXLOG_WARN("XamContentLaunchImageInternal: no launcher title in the arguments");
  }
  ctx.r3.u64 = 0x80004005;  // E_FAIL
}

REX_HOOK_RAW(__imp__XamLoaderLaunchTitleEx) {
  (void)base;
  auto* memory = REX_KERNEL_MEMORY();
  std::string path =
      ctx.r3.u32 ? std::string(memory->TranslateVirtual<const char*>(ctx.r3.u32)) : "";
  REXLOG_INFO("XamLoaderLaunchTitleEx(path='{}', r4={:08X}, r5={:08X}, r6={:08X})", path,
              ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  ctx.r3.u64 = 0x80004005;  // E_FAIL
}
