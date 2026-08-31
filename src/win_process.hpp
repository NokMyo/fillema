#pragma once

#if !defined(_WIN32)
#error win_process.hpp is only available on Windows.
#endif

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fillema {

struct ProcessResult {
    bool started = false;
    DWORD exitCode = static_cast<DWORD>(-1);
    std::string output;
    std::wstring error;
};

[[nodiscard]] std::wstring Utf8ToWide(std::string_view value);
[[nodiscard]] std::string WideToUtf8(std::wstring_view value);
[[nodiscard]] std::filesystem::path ExecutableDirectory();
[[nodiscard]] std::filesystem::path FindBundledTool(std::wstring_view fileName);
[[nodiscard]] ProcessResult RunHiddenProcess(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    bool captureOutput = true);

} // namespace fillema

