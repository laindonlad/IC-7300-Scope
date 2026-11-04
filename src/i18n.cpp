#include "i18n.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <cstdlib>

#include "builtin_lang.inc"

namespace {

struct Table {
  std::unordered_map<std::string, std::string> text;
  std::unordered_map<std::string_view, const char*> index;  // into text, which doesn't move its nodes
};

// What printf will read for a format: each conversion's * widths, length and letter. A translation has to
// match its English exactly, or printf would read the arguments wrongly.
bool formatShape(const std::string& s, std::string& shape) {
  shape.clear();
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '%') continue;
    if (++i >= s.size()) return false;
    if (s[i] == '%') continue;
    while (i < s.size() && strchr("-+ #0'", s[i])) i++;
    if (i < s.size() && s[i] == '*') shape += '*', i++;
    while (i < s.size() && isdigit((unsigned char)s[i])) i++;
    if (i < s.size() && s[i] == '$') return false;  // no reordering
    if (i < s.size() && s[i] == '.') {
      i++;
      if (i < s.size() && s[i] == '*') shape += '*', i++;
      while (i < s.size() && isdigit((unsigned char)s[i])) i++;
    }
    while (i < s.size() && strchr("hlLzjtq", s[i])) shape += s[i++];
    if (i >= s.size() || !strchr("diouxXeEfFgGaAcspn", s[i])) return false;
    shape += s[i];
    shape += ',';
  }
  return true;
}

std::string unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char c = s[++i];
      out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
    } else {
      out += s[i];
    }
  }
  return out;
}

// The file is pairs of lines: the English, then "= " and the translation. # starts a comment line.
std::unique_ptr<Table> parse(const char* data, size_t size) {
  auto t = std::make_unique<Table>();
  std::string_view all(data, size);
  std::string key;
  bool haveKey = false;
  size_t pos = 0;
  if (all.substr(0, 3) == "\xEF\xBB\xBF") pos = 3;
  while (pos < all.size()) {
    size_t end = all.find('\n', pos);
    if (end == std::string_view::npos) end = all.size();
    std::string_view line = all.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '=') {
      if (!haveKey) continue;
      line.remove_prefix(1);
      if (!line.empty() && line[0] == ' ') line.remove_prefix(1);
      std::string value = unescape(line);
      std::string a, b;
      if (!value.empty() && formatShape(key, a) && formatShape(value, b) && a == b) t->text[key] = value;
      else fprintf(stderr, "i18n: dropped the translation of \"%s\": it doesn't match the English's %% fields\n", key.c_str());
      haveKey = false;
    } else {
      key = unescape(line);
      haveKey = true;
    }
  }
  for (auto& kv : t->text) t->index.emplace(std::string_view(kv.first), kv.second.c_str());
  return t;
}

const std::vector<i18n::Language> kLanguages = {
    {"en", "English"},   {"de", "Deutsch"},   {"fr", "Fran\xC3\xA7" "ais"}, {"es", "Espa\xC3\xB1ol"},
    {"it", "Italiano"},  {"pt", "Portugu\xC3\xAAs"}, {"nl", "Nederlands"}, {"pl", "Polski"},
    {"cs", "\xC4\x8C" "e\xC5\xA1tina"},
    {"ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9"},
    {"uk", "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0"},
    {"el", "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC"},
};

std::mutex gLoad;
std::map<std::string, std::unique_ptr<Table>> gTables;  // loaded once each and kept, so other threads can read
std::atomic<const Table*> gCurrent{nullptr};
std::string gCode = "en";
std::unordered_map<std::string, std::string> gIds;  // trId's composed labels, for the main thread

bool known(const std::string& code) {
  for (const auto& l : kLanguages)
    if (code == l.code) return true;
  return false;
}

// "de-DE", "de_DE.UTF-8", "pt-BR" and so on: our code for it, or "".
std::string ours(std::string tag) {
  std::string code;
  for (char c : tag) {
    if (!isalpha((unsigned char)c)) break;
    code += (char)tolower((unsigned char)c);
  }
  return known(code) ? code : "";
}

}  // namespace

namespace i18n {

const std::vector<Language>& languages() { return kLanguages; }

std::string systemLanguage() {
#ifdef _WIN32
  ULONG count = 0, size = 0;
  if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size > 0) {
    std::vector<wchar_t> buf(size);
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size)) {
      for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::string tag;
        for (const wchar_t* q = p; *q; q++) tag += (char)(*q < 128 ? *q : '?');
        std::string c = ours(tag);
        if (!c.empty()) return c;
        if (tag.compare(0, 2, "en") == 0) return "en";
      }
    }
  }
#elif defined(__APPLE__)
  std::string found = "en";
  if (CFArrayRef langs = CFLocaleCopyPreferredLanguages()) {
    for (CFIndex i = 0; i < CFArrayGetCount(langs); i++) {
      char tag[64];
      if (!CFStringGetCString((CFStringRef)CFArrayGetValueAtIndex(langs, i), tag, sizeof tag, kCFStringEncodingUTF8)) continue;
      std::string c = ours(tag);
      if (!c.empty()) {
        found = c;
        break;
      }
      if (strncmp(tag, "en", 2) == 0) break;
    }
    CFRelease(langs);
  }
  return found;
#else
  // LANGUAGE is a list in order of preference; the others name one locale.
  if (const char* l = getenv("LANGUAGE")) {
    std::string list = l;
    size_t pos = 0;
    while (pos <= list.size()) {
      size_t end = list.find(':', pos);
      if (end == std::string::npos) end = list.size();
      std::string tag = list.substr(pos, end - pos);
      std::string c = ours(tag);
      if (!c.empty()) return c;
      if (tag.compare(0, 2, "en") == 0) return "en";
      pos = end + 1;
    }
  }
  for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"})
    if (const char* v = getenv(var); v && *v) {
      std::string c = ours(v);
      return c.empty() ? "en" : c;
    }
#endif
  return "en";
}

std::string use(const std::string& choice) {
  std::string code = choice.empty() || choice == "auto" ? systemLanguage() : choice;
  if (!known(code)) code = "en";
  std::lock_guard<std::mutex> lock(gLoad);
  const Table* t = nullptr;
  if (code != "en") {
    auto it = gTables.find(code);
    if (it == gTables.end()) {
      std::unique_ptr<Table> loaded;
      for (const auto& f : kBuiltinLang)
        if (code == f.id) loaded = parse((const char*)f.data, f.size);
      it = gTables.emplace(code, loaded ? std::move(loaded) : std::make_unique<Table>()).first;
    }
    t = it->second.get();
  }
  gCurrent.store(t);
  gCode = code;
  gIds.clear();
  return code;
}

const std::string& current() { return gCode; }
bool needsGreek() { return gCode == "el"; }
bool needsCyrillic() { return gCode == "ru" || gCode == "uk"; }

}  // namespace i18n

const char* tr(const char* en) {
  const Table* t = gCurrent.load(std::memory_order_relaxed);
  if (!t || !en || !*en) return en;
  auto it = t->index.find(std::string_view(en));
  return it == t->index.end() ? en : it->second;
}

const char* trId(const char* label) {
  const Table* t = gCurrent.load(std::memory_order_relaxed);
  if (!t || !label || !*label) return label;
  auto c = gIds.find(label);
  if (c != gIds.end()) return c->second.c_str();
  const char* hash = strstr(label, "##");
  std::string_view shown = hash ? std::string_view(label, (size_t)(hash - label)) : std::string_view(label);
  auto it = shown.empty() ? t->index.end() : t->index.find(shown);
  if (it == t->index.end()) return label;  // not cached, as labels with numbers in them come and go
  std::string out = it->second;
  // ### makes the ID from what follows it alone, so the ID is the English label's whatever is shown.
  if (hash && strncmp(hash, "###", 3) == 0) out += hash;
  else out += std::string("###") + label;
  return gIds.emplace(label, std::move(out)).first->second.c_str();
}

std::string trf(const char* fmt, ...) {
  const char* f = tr(fmt);
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, f, ap);
  va_end(ap);
  std::string out;
  if (n > 0) {
    out.resize((size_t)n + 1);
    vsnprintf(&out[0], out.size(), f, ap2);
    out.resize((size_t)n);
  }
  va_end(ap2);
  return out;
}
