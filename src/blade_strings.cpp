// blade - dashboard text from strings.toml.
//
// Where the text lives: dash.xex carries its UI as XEX resource sections
// (dashmain, games, gamesbla, ...), each an XUIZ package (the .xzp format):
//   <scene>.xur               XUIB binary scene. English text is built in: it
//                             sits in the scene's STRN string table next to
//                             class and element names (XuiLabel, btnLaunch).
//   <locale>\<scene>.xus      XUIS v0x0101: that scene's translations, entries
//                             {text, u16 0x0108, u16 id}; the id is the scene's
//                             own localisation id, not a STRN index.
//   [<locale>\]Strings.xus    XUIS v0x0102: text the code looks up by position;
//                             an empty entry means "not translated".
// The dashboard reads every section through XexGetModuleSection (one call
// site, sub_920F7968, the XUI section:// loader). Blade overrides that import
// and hands back a rebuilt package with the strings from strings.toml, so
// replacements can be any length and each language is configured separately.
//
// strings.toml (next to the executable, else next to the game folder):
//   [en]                              # English (the built-in scene text and
//   "Demos" = "Games"                 #   the English Strings.xus)
//   [de-de]                           # a translation table
//   "Demo spielen" = "Spiel starten"
//   [en."games/405_DemoOptions"]      # only in one scene (section/scene)
//   "Demos" = "Games"
// A language with no translation of a scene string shows the English text,
// so [en] changes reach it too.
//
// --blade_extract_strings=<file> writes every string as a commented template.
// --blade_dump_resources=<dir> writes the sections as loaded (<name>.bin).

#include "blade_strings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/util/xex2_info.h>
#include <rex/system/xmemory.h>
#include <rex/system/xmodule.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

REXCVAR_DEFINE_STRING(blade_strings_config, "", "Blade",
                      "Dashboard text overrides (default: strings.toml next to the executable, "
                      "else next to the game folder)");
REXCVAR_DEFINE_STRING(blade_extract_strings, "", "Blade",
                      "Write every dashboard string to this file as a strings.toml template");
REXCVAR_DEFINE_STRING(blade_dump_resources, "", "Blade",
                      "Write the XEX's embedded resource sections, as loaded, to this folder "
                      "(<name>.bin)");

namespace blade {
namespace {

using Bytes = std::vector<uint8_t>;

//-----------------------------------------------------------------------------
// Big-endian helpers
//-----------------------------------------------------------------------------

struct Reader {
  const uint8_t* p;
  size_t size;
  size_t pos = 0;
  bool ok = true;

  bool Need(size_t n) {
    if (pos + n > size) ok = false;
    return ok;
  }
  uint8_t U8() { return Need(1) ? p[pos++] : 0; }
  uint16_t U16() {
    if (!Need(2)) return 0;
    uint16_t v = uint16_t(p[pos] << 8 | p[pos + 1]);
    pos += 2;
    return v;
  }
  uint32_t U32() {
    if (!Need(4)) return 0;
    uint32_t v = uint32_t(p[pos]) << 24 | uint32_t(p[pos + 1]) << 16 |
                 uint32_t(p[pos + 2]) << 8 | p[pos + 3];
    pos += 4;
    return v;
  }
  std::u16string Str16(size_t chars) {
    std::u16string s;
    if (!Need(chars * 2)) return s;
    for (size_t i = 0; i < chars; ++i) {
      s.push_back(char16_t(p[pos + 2 * i] << 8 | p[pos + 2 * i + 1]));
    }
    pos += chars * 2;
    return s;
  }
  Bytes Raw(size_t n) {
    if (!Need(n)) return {};
    Bytes b(p + pos, p + pos + n);
    pos += n;
    return b;
  }
};

void Put16(Bytes& b, uint16_t v) {
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
void Put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}
void PutStr16(Bytes& b, std::u16string_view s) {
  Put16(b, uint16_t(s.size()));
  for (char16_t c : s) Put16(b, uint16_t(c));
}
void Append(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

//-----------------------------------------------------------------------------
// XUIZ package: 'XUIZ' u32 ver, u32 total, u32 0, u32 dir_size, u16 count;
// entries {u32 size, u32 offset, u8 name_chars, UTF-16 name}; file data
// follows the directory, offsets relative to it.
//-----------------------------------------------------------------------------

struct XuizFile {
  std::u16string name;
  uint32_t offset;
  Bytes data;
};
struct Xuiz {
  uint32_t version = 0;
  uint32_t reserved = 0;
  std::vector<XuizFile> files;
};

std::optional<Xuiz> ParseXuiz(const uint8_t* p, size_t size) {
  if (size < 22 || std::memcmp(p, "XUIZ", 4) != 0) return std::nullopt;
  Reader r{p, size, 4};
  Xuiz z;
  z.version = r.U32();
  r.U32();  // total size
  z.reserved = r.U32();
  uint32_t dir_size = r.U32();
  uint16_t count = r.U16();
  struct Entry {
    std::u16string name;
    uint32_t size, offset;
  };
  std::vector<Entry> entries;
  for (uint16_t i = 0; i < count && r.ok; ++i) {
    Entry e;
    e.size = r.U32();
    e.offset = r.U32();
    e.name = r.Str16(r.U8());
    entries.push_back(std::move(e));
  }
  size_t data0 = 22 + size_t(dir_size);
  if (!r.ok || r.pos != data0) return std::nullopt;
  for (auto& e : entries) {
    if (data0 + e.offset + e.size > size) return std::nullopt;
    z.files.push_back({std::move(e.name), e.offset,
                       Bytes(p + data0 + e.offset, p + data0 + e.offset + e.size)});
  }
  return z;
}

Bytes BuildXuiz(const Xuiz& z) {
  // Keep the directory order and the original data order.
  std::vector<size_t> by_offset(z.files.size());
  for (size_t i = 0; i < by_offset.size(); ++i) by_offset[i] = i;
  std::stable_sort(by_offset.begin(), by_offset.end(),
                   [&](size_t a, size_t b) { return z.files[a].offset < z.files[b].offset; });
  std::vector<uint32_t> offsets(z.files.size());
  Bytes data;
  for (size_t i : by_offset) {
    offsets[i] = uint32_t(data.size());
    Append(data, z.files[i].data);
  }
  Bytes dir;
  for (size_t i = 0; i < z.files.size(); ++i) {
    const auto& f = z.files[i];
    Put32(dir, uint32_t(f.data.size()));
    Put32(dir, offsets[i]);
    dir.push_back(uint8_t(f.name.size()));
    for (char16_t c : f.name) Put16(dir, uint16_t(c));
  }
  Bytes out{'X', 'U', 'I', 'Z'};
  Put32(out, z.version);
  Put32(out, uint32_t(22 + dir.size() + data.size()));
  Put32(out, z.reserved);
  Put32(out, uint32_t(dir.size()));
  Put16(out, uint16_t(z.files.size()));
  Append(out, dir);
  Append(out, data);
  return out;
}

//-----------------------------------------------------------------------------
// XUIB scene: 'XUIB' u32 ver, u32 flags, u16, u32 total, u16 sections;
// flags & 1: a 0x28-byte block of object counts; then sections {4cc, u32
// offset, u32 size}. STRN is a run of {u16 chars, UTF-16}.
//-----------------------------------------------------------------------------

struct XuibSection {
  char tag[4];
  Bytes data;
};
struct Xuib {
  uint32_t version = 0, flags = 0;
  uint16_t unknown = 0;
  Bytes counts;
  std::vector<XuibSection> sections;
};

std::optional<Xuib> ParseXuib(const Bytes& b) {
  if (b.size() < 20 || std::memcmp(b.data(), "XUIB", 4) != 0) return std::nullopt;
  Reader r{b.data(), b.size(), 4};
  Xuib x;
  x.version = r.U32();
  x.flags = r.U32();
  x.unknown = r.U16();
  if (r.U32() != b.size()) return std::nullopt;
  uint16_t n = r.U16();
  if (x.flags & 1) x.counts = r.Raw(0x28);
  struct Entry {
    char tag[4];
    uint32_t offset, size;
  };
  std::vector<Entry> entries(n);
  for (auto& e : entries) {
    if (!r.Need(12)) return std::nullopt;
    std::memcpy(e.tag, b.data() + r.pos, 4);
    r.pos += 4;
    e.offset = r.U32();
    e.size = r.U32();
  }
  // The rebuild lays sections out back to back after the header; accept only
  // scenes that already look like that, so nothing between them is lost.
  uint32_t expect = uint32_t(r.pos);
  for (auto& e : entries) {
    if (!r.ok || e.offset != expect || size_t(e.offset) + e.size > b.size()) return std::nullopt;
    XuibSection s;
    std::memcpy(s.tag, e.tag, 4);
    s.data.assign(b.begin() + e.offset, b.begin() + e.offset + e.size);
    x.sections.push_back(std::move(s));
    expect += e.size;
  }
  if (expect != b.size()) return std::nullopt;
  return x;
}

Bytes BuildXuib(const Xuib& x) {
  uint32_t header = uint32_t(20 + x.counts.size() + 12 * x.sections.size());
  uint32_t total = header;
  for (const auto& s : x.sections) total += uint32_t(s.data.size());
  Bytes out{'X', 'U', 'I', 'B'};
  Put32(out, x.version);
  Put32(out, x.flags);
  Put16(out, x.unknown);
  Put32(out, total);
  Put16(out, uint16_t(x.sections.size()));
  Append(out, x.counts);
  uint32_t at = header;
  for (const auto& s : x.sections) {
    out.insert(out.end(), s.tag, s.tag + 4);
    Put32(out, at);
    Put32(out, uint32_t(s.data.size()));
    at += uint32_t(s.data.size());
  }
  for (const auto& s : x.sections) Append(out, s.data);
  return out;
}

std::optional<std::vector<std::u16string>> ParseStrn(const Bytes& b) {
  Reader r{b.data(), b.size()};
  std::vector<std::u16string> out;
  while (r.ok && r.pos < b.size()) out.push_back(r.Str16(r.U16()));
  if (!r.ok) return std::nullopt;
  return out;
}

Bytes BuildStrn(const std::vector<std::u16string>& strs) {
  Bytes out;
  for (const auto& s : strs) PutStr16(out, s);
  return out;
}

//-----------------------------------------------------------------------------
// XUIS string table: 'XUIS' u16 ver, u32 total, u16 count; entries
// {u16 chars, UTF-16} plus, for v0x0101, {u16 flags, u16 id}.
//-----------------------------------------------------------------------------

struct XuisEntry {
  std::u16string text;
  uint16_t flags = 0, id = 0;
};
struct Xuis {
  uint16_t version = 0;
  std::vector<XuisEntry> entries;
};

std::optional<Xuis> ParseXuis(const Bytes& b) {
  if (b.size() < 12 || std::memcmp(b.data(), "XUIS", 4) != 0) return std::nullopt;
  Reader r{b.data(), b.size(), 4};
  Xuis x;
  x.version = r.U16();
  if (x.version != 0x0101 && x.version != 0x0102) return std::nullopt;
  if (r.U32() != b.size()) return std::nullopt;
  uint16_t count = r.U16();
  for (uint16_t i = 0; i < count && r.ok; ++i) {
    XuisEntry e;
    e.text = r.Str16(r.U16());
    if (x.version == 0x0101) {
      e.flags = r.U16();
      e.id = r.U16();
    }
    x.entries.push_back(std::move(e));
  }
  if (!r.ok || r.pos != b.size()) return std::nullopt;
  return x;
}

Bytes BuildXuis(const Xuis& x) {
  Bytes body;
  for (const auto& e : x.entries) {
    PutStr16(body, e.text);
    if (x.version == 0x0101) {
      Put16(body, e.flags);
      Put16(body, e.id);
    }
  }
  Bytes out{'X', 'U', 'I', 'S'};
  Put16(out, x.version);
  Put32(out, uint32_t(12 + body.size()));
  Put16(out, uint16_t(x.entries.size()));
  Append(out, body);
  return out;
}

//-----------------------------------------------------------------------------
// Configuration and lookup
//-----------------------------------------------------------------------------

using TextMap = std::map<std::u16string, std::u16string>;
struct LocaleTable {
  TextMap global;
  std::map<std::string, TextMap> scoped;  // lower-case "section/scene"
};
std::map<std::string, LocaleTable> g_tables;  // "en", "de-de", ...

void LoadConfig(const std::filesystem::path& game_data_root) {
  std::vector<std::filesystem::path> candidates;
  std::string configured = REXCVAR_GET(blade_strings_config);
  if (!configured.empty()) {
    candidates.emplace_back(configured);
  } else {
    candidates.push_back(rex::filesystem::GetExecutableFolder() / "strings.toml");
    candidates.push_back(std::filesystem::absolute(game_data_root).parent_path() /
                         "strings.toml");
  }
  for (const auto& path : candidates) {
    if (!std::filesystem::exists(path)) continue;
    toml::table root;
    try {
      root = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
      REXLOG_ERROR("Strings: {}: {}", path.string(), e.what());
      return;
    }
    size_t total = 0;
    auto read_map = [&](const toml::table& t, TextMap& out) {
      for (auto& [k, v] : t) {
        if (auto s = v.value<std::string>()) {
          out[rex::string::to_utf16(k.str())] = rex::string::to_utf16(*s);
          ++total;
        }
      }
    };
    for (auto& [locale, node] : root) {
      auto* t = node.as_table();
      if (!t) continue;
      auto& table = g_tables[Lower(std::string(locale.str()))];
      read_map(*t, table.global);
      for (auto& [scope, sub] : *t) {
        if (auto* st = sub.as_table()) {
          read_map(*st, table.scoped[Lower(std::string(scope.str()))]);
        }
      }
    }
    REXLOG_INFO("Strings: {} override(s) in {} language(s) from {}", total, g_tables.size(),
                path.string());
    return;
  }
  REXLOG_INFO("Strings: no strings.toml found; dashboard text unchanged");
}

const std::u16string* Lookup(const std::string& locale, const std::string& scope,
                             const std::u16string& text) {
  auto t = g_tables.find(locale);
  if (t == g_tables.end() || text.empty()) return nullptr;
  if (auto s = t->second.scoped.find(scope); s != t->second.scoped.end()) {
    if (auto it = s->second.find(text); it != s->second.end()) return &it->second;
  }
  if (auto it = t->second.global.find(text); it != t->second.global.end()) return &it->second;
  return nullptr;
}

// "de-de\405_DemoOptions.xus" -> locale "de-de", scene "405_DemoOptions", ".xus".
void SplitName(const std::u16string& name16, std::string* locale, std::string* scene,
               std::string* ext) {
  std::string name = rex::string::to_utf8(name16);
  auto slash = name.find_last_of("\\/");
  *locale = slash == std::string::npos ? "en" : Lower(name.substr(0, slash));
  std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
  auto dot = base.rfind('.');
  *scene = dot == std::string::npos ? base : base.substr(0, dot);
  *ext = dot == std::string::npos ? "" : Lower(base.substr(dot));
}

//-----------------------------------------------------------------------------
// Extraction template
//-----------------------------------------------------------------------------

// A scene's STRN also holds class names, element names and paths. Leave the
// ones that look like that out of the template: renaming them could break
// code that looks an element up by name. Single capitalised words ("Demos")
// stay, since labels often look like that.
bool LooksLikeIdentifier(const std::u16string& s16) {
  std::string s = rex::string::to_utf8(s16);
  if (s.empty()) return true;
  if (s.find(' ') != std::string::npos) return false;
  if (s.find("://") != std::string::npos || s.find('_') != std::string::npos) return true;
  auto dot = s.rfind('.');
  if (dot != std::string::npos && s.size() - dot <= 4 && dot + 1 < s.size()) return true;
  if (s.rfind("Xui", 0) == 0) return true;
  if (std::none_of(s.begin(), s.end(), [](unsigned char c) { return std::isalpha(c); })) {
    return true;
  }
  unsigned char c0 = s[0];
  if (std::islower(c0)) return true;  // btnLaunch, legend
  if (c0 == 'C' && s.size() > 1 && std::isupper((unsigned char)s[1])) return true;  // CDemoScene
  return false;
}

std::string TomlQuote(const std::u16string& s16) {
  std::string s = rex::string::to_utf8(s16), out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04X", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  return out + "\"";
}

struct Extract {
  // scope -> locale -> strings in first-seen order
  std::map<std::string, std::map<std::string, std::vector<std::u16string>>> by_scope;
  std::vector<std::string> scope_order;
  void Add(const std::string& scope, const std::string& locale, const std::u16string& text) {
    auto [it, inserted] = by_scope.try_emplace(scope);
    if (inserted) scope_order.push_back(scope);
    auto& v = it->second[locale];
    if (std::find(v.begin(), v.end(), text) == v.end()) v.push_back(text);
  }
};

void WriteExtract(const Extract& x, const std::string& path) {
  std::ofstream out(path, std::ios::binary);
  out << "# Blade dashboard strings, extracted from dash.xex.\n"
         "#\n"
         "# Copy the lines you want into strings.toml and edit the right-hand side.\n"
         "# Each table here is one scene (section/scene) in one language. A line put\n"
         "# under a plain [en] or [de-de] table instead applies in every scene.\n"
         "#\n"
         "# en is the text built into the scenes plus the English Strings.xus; the\n"
         "# other languages are their translation tables, which only list what they\n"
         "# translate (anything else shows the English text). Scene string tables\n"
         "# also hold class and element names; ones that look like names (XuiLabel,\n"
         "# btnLaunch, Label_Head, paths) are left out, since renaming them could\n"
         "# break the dashboard code that looks them up.\n";
  size_t n = 0;
  for (const auto& scope : x.scope_order) {
    const auto& locales = x.by_scope.at(scope);
    out << "\n# ==== " << scope << " ====\n";
    for (const auto& [locale, strs] : locales) {
      out << "\n[" << locale << ".\"" << scope << "\"]\n";
      for (const auto& s : strs) {
        out << "# " << TomlQuote(s) << " = " << TomlQuote(s) << "\n";
        ++n;
      }
    }
  }
  REXLOG_INFO("Strings: wrote {} string(s) from {} scene(s) to {}", n, x.scope_order.size(),
              path);
}

//-----------------------------------------------------------------------------
// Section processing
//-----------------------------------------------------------------------------

struct Patched {
  uint32_t address, size;
};
std::map<std::string, Patched> g_patched;  // lower-case section name

// Applies the overrides to one package; returns the number of strings changed.
size_t ProcessPackage(const std::string& section, Xuiz& z, Extract* extract) {
  size_t changed = 0;
  for (auto& f : z.files) {
    std::string locale, scene, ext;
    SplitName(f.name, &locale, &scene, &ext);
    std::string display_scope = section + "/" + scene;
    std::string scope = Lower(display_scope);

    if (ext == ".xur") {
      auto x = ParseXuib(f.data);
      if (!x) {
        if (f.data.size() >= 4 && std::memcmp(f.data.data(), "XUIB", 4) == 0) {
          REXLOG_WARN("Strings: {}: unrecognised scene layout, left as is", display_scope);
        }
        continue;
      }
      for (auto& s : x->sections) {
        if (std::memcmp(s.tag, "STRN", 4) != 0) continue;
        auto strs = ParseStrn(s.data);
        if (!strs) break;
        size_t before = changed;
        for (auto& str : *strs) {
          if (extract && !LooksLikeIdentifier(str)) extract->Add(display_scope, locale, str);
          if (auto* to = Lookup(locale, scope, str)) {
            str = *to;
            ++changed;
          }
        }
        if (changed != before) {
          s.data = BuildStrn(*strs);
          f.data = BuildXuib(*x);
        }
        break;
      }
    } else if (ext == ".xus") {
      auto x = ParseXuis(f.data);
      if (!x) continue;
      size_t before = changed;
      for (auto& e : x->entries) {
        if (e.text.empty()) continue;
        if (extract) extract->Add(display_scope, locale, e.text);
        if (auto* to = Lookup(locale, scope, e.text)) {
          e.text = *to;
          ++changed;
        }
      }
      if (changed != before) f.data = BuildXuis(*x);
    }
  }
  return changed;
}

}  // namespace

std::optional<std::pair<uint32_t, uint32_t>> FindRebuiltSection(const std::string& name) {
  auto it = g_patched.find(Lower(name));
  if (it == g_patched.end()) return std::nullopt;
  return std::pair{it->second.address, it->second.size};
}

void PrepareDashStrings(const std::filesystem::path& game_data_root) {
  LoadConfig(game_data_root);
  std::string extract_path = REXCVAR_GET(blade_extract_strings);
  std::string dump_dir = REXCVAR_GET(blade_dump_resources);
  if (g_tables.empty() && extract_path.empty() && dump_dir.empty()) return;

  auto module = REX_KERNEL_STATE()->GetExecutableModule();
  rex::xex2_opt_resource_info* info = nullptr;
  if (!module || XFAILED(module->GetOptHeader(rex::XEX_HEADER_RESOURCE_INFO, &info)) || !info) {
    REXLOG_WARN("Strings: no resource info in the executable");
    return;
  }
  auto* memory = rex::system::kernel_memory();
  uint32_t count = (uint32_t(info->size) - 4) / sizeof(rex::xex2_resource);
  Extract extract;
  size_t total = 0;

  for (uint32_t i = 0; i < count; ++i) {
    const auto& res = info->resources[i];
    std::string name(res.name, strnlen(res.name, sizeof(res.name)));
    const uint8_t* data = memory->TranslateVirtual<const uint8_t*>(uint32_t(res.address));
    uint32_t size = res.size;

    if (!dump_dir.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(dump_dir, ec);
      auto path = std::filesystem::path(dump_dir) / (name + ".bin");
      std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data), size);
    }

    auto z = ParseXuiz(data, size);
    if (!z) continue;  // FFFE07D1 is the XDBF title data
    size_t changed = ProcessPackage(name, *z, extract_path.empty() ? nullptr : &extract);
    if (!changed) continue;

    Bytes rebuilt = BuildXuiz(*z);
    uint32_t guest = memory->SystemHeapAlloc(uint32_t(rebuilt.size()));
    if (!guest) {
      REXLOG_ERROR("Strings: no memory for the rebuilt {} ({} bytes)", name, rebuilt.size());
      continue;
    }
    std::memcpy(memory->TranslateVirtual<uint8_t*>(guest), rebuilt.data(), rebuilt.size());
    g_patched[Lower(name)] = {guest, uint32_t(rebuilt.size())};
    REXLOG_INFO("Strings: {}: {} string(s) changed ({} -> {} bytes, at {:08X})", name, changed,
                size, rebuilt.size(), guest);
    total += changed;
  }
  if (!dump_dir.empty()) REXLOG_INFO("Strings: dumped {} section(s) to {}", count, dump_dir);
  if (!extract_path.empty()) WriteExtract(extract, extract_path);
  REXLOG_INFO("Strings: {} string(s) changed in {} section(s)", total, g_patched.size());
}

}  // namespace blade

//=============================================================================
// XexGetModuleSection override: the dashboard's own sections come from the
// rebuilt copies when strings.toml changed them. Same symbol as the SDK's
// REX_EXPORT; the executable's definition takes precedence.
//=============================================================================

namespace {
using namespace rex;
using namespace rex::system;

u32 BladeXexGetModuleSection(mapped_void hmodule, mapped_string name, mapped_u32 data_ptr,
                             mapped_u32 size_ptr) {
  auto module = XModule::GetFromHModule(REX_KERNEL_STATE(), hmodule);
  if (!module) return X_STATUS_INVALID_HANDLE;
  std::string section(name.value());

  auto exe = REX_KERNEL_STATE()->GetExecutableModule();
  if (exe && static_cast<XModule*>(exe.get()) == module.get()) {
    if (auto rebuilt = blade::FindRebuiltSection(section)) {
      *data_ptr = rebuilt->first;
      *size_ptr = rebuilt->second;
      return X_STATUS_SUCCESS;
    }
  }
  uint32_t data = 0, size = 0;
  X_STATUS result = module->GetSection(section, &data, &size);
  if (XSUCCEEDED(result)) {
    *data_ptr = data;
    *size_ptr = size;
  }
  return result;
}
}  // namespace

REX_HOOK(__imp__XexGetModuleSection, BladeXexGetModuleSection)
