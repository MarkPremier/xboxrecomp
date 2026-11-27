#pragma once
/* Host adapters for DXVK-native. This translation unit uses DXVK's Win32
 * vocabulary, independently of the kernel's POSIX compatibility vocabulary. */
#include <windows.h>
#include <d3dcompiler.h>
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <climits>
#include <atomic>
#include <chrono>
#include <codecvt>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <locale>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include <strings.h>
#include <dlfcn.h>

namespace Microsoft::WRL {
template<class T> class ComPtr {
    T *value = nullptr;
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ComPtr(T *p) : value(p) { if (value) value->AddRef(); }
    ComPtr(const ComPtr &p) : ComPtr(p.value) {}
    ComPtr(ComPtr &&p) noexcept : value(std::exchange(p.value, nullptr)) {}
    ~ComPtr() { Reset(); }
    ComPtr &operator=(const ComPtr &p) { if (this != std::addressof(p)) { ComPtr copy(p); std::swap(value, copy.value); } return *this; }
    ComPtr &operator=(ComPtr &&p) noexcept { if (this != std::addressof(p)) { Reset(); value = std::exchange(p.value, nullptr); } return *this; }
    ComPtr &operator=(std::nullptr_t) { Reset(); return *this; }
    T *Get() const { return value; }
    T *operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
    bool operator==(std::nullptr_t) const { return !value; }
    bool operator!=(std::nullptr_t) const { return value; }
    template<class U> bool operator==(const ComPtr<U> &other) const { return value == other.Get(); }
    T **GetAddressOf() { return &value; }
    T **ReleaseAndGetAddressOf() { Reset(); return &value; }
    T **operator&() { return ReleaseAndGetAddressOf(); }
    void Reset() { if (T *p = std::exchange(value, nullptr)) p->Release(); }
    template<class U> HRESULT As(U **out) const { return value ? value->QueryInterface(__uuidof(U), reinterpret_cast<void **>(out)) : E_NOINTERFACE; }
};
}

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#undef MAX_PATH
#define MAX_PATH 4096
#define E_PENDING ((HRESULT)0x8000000au)
#define D3D11_FILTER_REDUCTION_TYPE_STANDARD 0
#define D3D11_MIP_LOD_BIAS_MAX 15.99f
#define CP_UTF8 65001
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define INVALID_FILE_ATTRIBUTES 0xffffffffu
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define MOVEFILE_REPLACE_EXISTING 1
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_BELOW_NORMAL -1
#ifndef HIWORD
#define HIWORD(x) ((uint16_t)((uint32_t)(x) >> 16))
#define LOWORD(x) ((uint16_t)(x))
#endif

static inline std::string native_path(const wchar_t *path) {
    std::string s = std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(path);
    for (char &c : s) if (c == '\\') c = '/';
    return s;
}
static inline ULONGLONG GetTickCount64() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static inline DWORD GetTickCount() { return static_cast<DWORD>(GetTickCount64()); }
static inline void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static inline void SwitchToThread() { std::this_thread::yield(); }
static inline HANDLE GetCurrentThread() { return nullptr; }
static inline DWORD GetCurrentThreadId() { return static_cast<DWORD>(gettid()); }
static inline DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
static inline void SetThreadPriority(HANDLE, int) {}
struct DEVMODEA { DWORD dmSize, dmPelsWidth, dmPelsHeight; };
static inline BOOL EnumDisplaySettingsA(const char *, DWORD, DEVMODEA *mode) {
    SDL_DisplayMode display;
    if (SDL_GetCurrentDisplayMode(0, &display)) return FALSE;
    mode->dmPelsWidth = display.w; mode->dmPelsHeight = display.h; return TRUE;
}
static inline BOOL GetClientRect(HWND window, RECT *rect) {
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window *>(window), &w, &h);
    *rect = {0, 0, w, h}; return TRUE;
}
static inline DWORD GetModuleFileNameA(HMODULE, char *path, DWORD size) {
    ssize_t length = readlink("/proc/self/exe", path, size ? size - 1 : 0);
    if (length < 0 || !size) return 0;
    path[length] = 0; return static_cast<DWORD>(length);
}
static inline int MultiByteToWideChar(unsigned, unsigned, const char *text, int count, wchar_t *out, int capacity) {
    std::wstring value = std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.from_bytes(text, text + (count < 0 ? std::strlen(text) : count));
    if (count < 0) value.push_back(0);
    if (!out) return static_cast<int>(value.size());
    if (value.size() > static_cast<size_t>(capacity)) return 0;
    std::copy(value.begin(), value.end(), out); return static_cast<int>(value.size());
}
static inline int WideCharToMultiByte(unsigned, unsigned, const wchar_t *text, int count, char *out, int capacity, void *, void *) {
    std::string value = std::wstring_convert<std::codecvt_utf8<wchar_t>>{}.to_bytes(text, text + (count < 0 ? std::wcslen(text) : count));
    if (count < 0) value.push_back(0);
    if (!out) return static_cast<int>(value.size());
    if (value.size() > static_cast<size_t>(capacity)) return 0;
    std::copy(value.begin(), value.end(), out); return static_cast<int>(value.size());
}
static inline DWORD GetModuleFileNameW(HMODULE module, wchar_t *path, DWORD size) {
    char bytes[MAX_PATH]; DWORD n = GetModuleFileNameA(module, bytes, sizeof bytes);
    int converted = n ? MultiByteToWideChar(CP_UTF8, 0, bytes, -1, path, size) : 0;
    return converted > 0 ? static_cast<DWORD>(converted - 1) : 0;
}
static inline DWORD GetEnvironmentVariableW(const wchar_t *name, wchar_t *out, DWORD size) {
    std::string key = native_path(name);
    const char *value = std::getenv(key == "LOCALAPPDATA" ? "XDG_CACHE_HOME" : key.c_str());
    std::string fallback;
    if (!value && key == "LOCALAPPDATA") {
        if (const char *home = std::getenv("HOME")) { fallback = std::string(home) + "/.cache"; value = fallback.c_str(); }
    }
    if (!value) return 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, value, -1, out, size);
    return n > 0 ? n - 1 : 0;
}
static inline BOOL CreateDirectoryW(const wchar_t *path, void *) { std::error_code ec; std::filesystem::create_directories(native_path(path), ec); return !ec; }
static inline DWORD GetFileAttributesW(const wchar_t *path) { struct stat st; if (stat(native_path(path).c_str(), &st)) return INVALID_FILE_ATTRIBUTES; return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0; }
static inline FILE *_wfopen(const wchar_t *path, const wchar_t *mode) { return std::fopen(native_path(path).c_str(), native_path(mode).c_str()); }
static inline HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD, void *, DWORD creation, DWORD, HANDLE) {
    FILE *f = std::fopen(native_path(path).c_str(), creation == CREATE_ALWAYS ? "wb" : access == GENERIC_READ ? "rb" : "r+b");
    return f ? f : INVALID_HANDLE_VALUE;
}
static inline BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) { struct stat st; if (fstat(fileno(static_cast<FILE *>(h)), &st)) return FALSE; size->QuadPart = st.st_size; return TRUE; }
static inline BOOL ReadFile(HANDLE h, void *data, DWORD size, DWORD *read, void *) { *read = std::fread(data, 1, size, static_cast<FILE *>(h)); return !std::ferror(static_cast<FILE *>(h)); }
static inline BOOL WriteFile(HANDLE h, const void *data, DWORD size, DWORD *written, void *) { *written = std::fwrite(data, 1, size, static_cast<FILE *>(h)); return *written == size; }
static inline BOOL CloseHandle(HANDLE h) { return !std::fclose(static_cast<FILE *>(h)); }
static inline BOOL MoveFileExW(const wchar_t *from, const wchar_t *to, DWORD) { return !std::rename(native_path(from).c_str(), native_path(to).c_str()); }
static inline BOOL DeleteFileW(const wchar_t *path) { return !std::remove(native_path(path).c_str()); }
static inline HMODULE LoadLibraryW(const wchar_t *path) { return dlopen(native_path(path).c_str(), RTLD_NOW | RTLD_LOCAL); }
static inline void *GetProcAddress(HMODULE module, const char *name) { return dlsym(module, name); }

/* vkd3d-utils exposes Windows-ABI COM objects. DXVK-native uses the host ABI,
 * so use vkd3d-shader's C API and own the resulting bytes in a native blob. */
HRESULT nv2a_native_compile(const void *source, size_t size, const char *name,
    const D3D_SHADER_MACRO *defines, ID3DInclude *include, const char *entry,
    const char *profile, UINT flags1, UINT flags2, ID3DBlob **code, ID3DBlob **errors);
HRESULT nv2a_native_blob(size_t size, ID3DBlob **blob);
#define D3DCreateBlob nv2a_native_blob
