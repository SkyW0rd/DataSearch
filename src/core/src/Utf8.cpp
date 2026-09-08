#include "datasearch/core/Utf8.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace datasearch::core {

#if defined(_WIN32)

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                            nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                           nullptr, nullptr);
    return out;
}

std::filesystem::path pathFromUtf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                            nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return std::filesystem::path(wide);
}

#else

std::string pathToUtf8(const std::filesystem::path& path) {
    return path.string();
}

std::filesystem::path pathFromUtf8(const std::string& utf8) {
    return std::filesystem::path(utf8);
}

#endif

} // namespace datasearch::core
