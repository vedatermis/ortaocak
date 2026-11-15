// Small Win32 helpers: UTF-8/UTF-16 conversion, HRESULT/GetLastError formatting, console attach.
#pragma once

#include <Windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace vo::client::platform {

[[nodiscard]] std::string toUtf8(std::wstring_view wide);
[[nodiscard]] std::wstring toWide(std::string_view utf8);

// "0x887A0005 (The GPU device instance has been suspended...)"
[[nodiscard]] std::string hresultToString(HRESULT hr);
[[nodiscard]] std::string lastErrorToString(DWORD error = ::GetLastError());

// Process command line as UTF-8 strings (argv[0] included).
[[nodiscard]] std::vector<std::string> commandLineArgsUtf8();

// Attaches stdout/stderr to the parent process console, if any (GUI-subsystem exes have none).
bool attachParentConsole();

} // namespace vo::client::platform
