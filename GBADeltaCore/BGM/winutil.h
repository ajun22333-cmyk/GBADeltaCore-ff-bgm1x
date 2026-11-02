// 한글 경로 처리를 위한 Windows 유틸리티 (UTF-8 <-> UTF-16)
#pragma once
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w((size_t) n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}

inline std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t) n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::vector<std::string> argsUtf8() {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> out;
    for (int i = 0; i < argc; ++i) out.push_back(wideToUtf8(wargv[i]));
    LocalFree(wargv);
    return out;
}

inline FILE* fopenUtf8(const std::string& path, const char* mode) {
    return _wfopen(utf8ToWide(path).c_str(), utf8ToWide(mode).c_str());
}

inline std::string exeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD) (sizeof(buf) / sizeof(buf[0])));
    std::wstring p(buf, n);
    size_t pos = p.find_last_of(L"\\/");
    return wideToUtf8(pos == std::wstring::npos ? L"." : p.substr(0, pos));
}
#else
inline std::vector<std::string> argsUtf8() { return {}; }
inline FILE* fopenUtf8(const std::string& path, const char* mode) { return std::fopen(path.c_str(), mode); }
inline std::string exeDir() { return "."; }
#endif
