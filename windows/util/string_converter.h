#pragma once

#include <string>

namespace util {
std::string Utf8FromUtf16(std::wstring_view utf16_string);
std::wstring Utf16FromUtf8(std::string_view utf8_string);

// Null-safe overloads. WebView2 callbacks routinely hand back a `LPCWSTR`
// that is null when the value is empty (e.g. ExecuteScript completing for
// a script with no return value, or URL-changed callbacks during early
// navigation). Constructing a `std::wstring_view` from a null pointer is
// undefined behavior — internally it would call `wcslen(nullptr)` — and
// the resulting bad-state `std::string` later corrupts the heap when it
// is destructed, crashing the process in `ucrtbase!free_base`
// (Sentry MOBILE-NEWS-A9 / -BB on this app's Windows build).
//
// These overloads check for null BEFORE building the view, so every
// callsite that passes a raw `LPCWSTR` / `LPCSTR` from a Win32 / WebView2
// API is automatically null-safe. Overload resolution picks them over
// the string_view forms when a raw pointer is passed.
std::string Utf8FromUtf16(const wchar_t* utf16_string);
std::wstring Utf16FromUtf8(const char* utf8_string);
}  // namespace util
