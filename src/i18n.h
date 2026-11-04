// The interface's languages. The English text is the key: tr("Connect") gives the chosen language's word for it,
// or "Connect" itself when there isn't one, so a missing translation shows English rather than nothing.
// The translations are lang/*.txt, built into the executable.
#pragma once
#include <string>
#include <vector>

namespace i18n {

struct Language {
  const char* code;  // "de"
  const char* name;  // "Deutsch", in its own language so anyone can find theirs
};

// English first, then the translations, in the order the settings list them.
const std::vector<Language>& languages();

// The two-letter code of the system's display language, or "en" if it isn't one of ours.
std::string systemLanguage();

// "" or "auto" follows the system. Returns the code now in use. Call between frames: the text it hands out
// before stays valid, but the labels change.
std::string use(const std::string& choice);
const std::string& current();

// Whether the language in use needs the Greek or Cyrillic letters in the fonts.
bool needsGreek();
bool needsCyrillic();

}  // namespace i18n

// Text to show. Also for printf formats: a translation must use the same %s and %d in the same order, or the
// English is kept.
const char* tr(const char* en);
// A widget's label. The translation shows, but the widget's ImGui ID stays the English one's, so changing
// the language doesn't upset anything ImGui remembers about it. "Name##x" translates "Name" and keeps "##x".
const char* trId(const char* label);
// Marks text that's translated later, where it's shown, so tools/lang_strings.py finds it.
#define N_(s) s
// tr() of a printf format, filled in.
std::string trf(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
