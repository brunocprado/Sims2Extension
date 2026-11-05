#pragma once

// ============================================================================
// i18n: tiny two-language (pt-BR / English) string layer for the F10 panel.
//
// The source language of every literal is Portuguese. Tr("texto em pt") returns the English
// translation when the panel is set to English and the original pointer otherwise (also when
// there is no table entry, so dynamic strings pass through untouched).
//
// Strings with printf placeholders are translated BEFORE formatting: Tr("%u moveis") -> "%u pieces".
// The choice is persisted in ts2lang.cfg (next to the game executable's working directory).
// ============================================================================
namespace ts2::i18n {

enum class Lang : int { PT = 0, EN = 1 };

Lang Get();
void Set(Lang l);               // also persists the choice
const char* Tr(const char* pt);

} // namespace ts2::i18n
