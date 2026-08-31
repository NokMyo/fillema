#include "win_process.hpp"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace fillema {
namespace {

class Handle final {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(value_, nullptr); }
    void reset(HANDLE value = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

std::wstring QuoteArgument(std::wstring_view argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(argument);
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring LastErrorText(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD count = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = count && buffer ? std::wstring(buffer, count) : L"Windows 오류 " + std::to_wstring(error);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}

} // namespace

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) return std::filesystem::current_path();
    buffer.resize(size);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path FindBundledTool(std::wstring_view fileName) {
    const std::filesystem::path base = ExecutableDirectory();
    const std::array candidates{
        base / "tools" / fileName,
        base / fileName,
        std::filesystem::path(fileName)
    };
    for (const auto& candidate : candidates) {
        if (candidate.is_absolute() && std::filesystem::exists(candidate)) return candidate;
        if (!candidate.is_absolute()) return candidate; // Let CreateProcess search PATH.
    }
    return std::filesystem::path(fileName);
}

ProcessResult RunHiddenProcess(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    bool captureOutput) {
    ProcessResult result;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle readPipe;
    Handle writePipe;
    if (captureOutput) {
        HANDLE readRaw = nullptr;
        HANDLE writeRaw = nullptr;
        if (!CreatePipe(&readRaw, &writeRaw, &security, 0)) {
            result.error = LastErrorText(GetLastError());
            return result;
        }
        readPipe.reset(readRaw);
        writePipe.reset(writeRaw);
        SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0);
    }

    std::wstring commandLine = QuoteArgument(executable.wstring());
    for (const auto& argument : arguments) {
        commandLine.push_back(L' ');
        commandLine += QuoteArgument(Utf8ToWide(argument));
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    Handle nullInput;
    if (captureOutput) {
        nullInput.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdOutput = writePipe.get();
        startup.hStdError = writePipe.get();
        startup.hStdInput = nullInput ? nullInput.get() : GetStdHandle(STD_INPUT_HANDLE);
    } else {
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
    }

    PROCESS_INFORMATION process{};
    const std::wstring executableString = executable.wstring();
    if (!CreateProcessW(
            executableString.find(L'\\') == std::wstring::npos ? nullptr : executableString.c_str(),
            mutableCommand.data(), nullptr, nullptr, captureOutput ? TRUE : FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        result.error = LastErrorText(GetLastError());
        return result;
    }
    result.started = true;
    Handle processHandle(process.hProcess);
    Handle threadHandle(process.hThread);
    writePipe.reset();

    if (captureOutput) {
        std::array<char, 4096> buffer{};
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &available, nullptr)) break;
            while (available > 0) {
                DWORD read = 0;
                const DWORD request = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
                if (!ReadFile(readPipe.get(), buffer.data(), request, &read, nullptr) || read == 0) break;
                result.output.append(buffer.data(), read);
                available -= read;
            }
            if (WaitForSingleObject(processHandle.get(), available > 0 ? 0 : 25) == WAIT_OBJECT_0) {
                while (ReadFile(readPipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &available, nullptr) && available > 0) {
                    result.output.append(buffer.data(), available);
                }
                break;
            }
        }
    } else {
        WaitForSingleObject(processHandle.get(), INFINITE);
    }
    GetExitCodeProcess(processHandle.get(), &result.exitCode);
    return result;
}

} // namespace fillema
