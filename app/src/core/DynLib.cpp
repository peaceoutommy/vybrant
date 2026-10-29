#include "core/DynLib.h"
#ifdef _WIN32
#include <QDir>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mt::dynlib {
#ifdef _WIN32
// the plugin's own DLLs next to it are found first; LoadLibrary wants backslashes for that
void* open(const QString& path) {
    return (void*)LoadLibraryExW((LPCWSTR)QDir::toNativeSeparators(path).utf16(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}
void* symbol(void* lib, const char* name) { return lib ? (void*)GetProcAddress((HMODULE)lib, name) : nullptr; }
// Plugin DLLs stay loaded: some leave a timer or window behind that Windows still calls after their editor and instances are
// gone, and that jumps into unmapped code once the DLL is freed (seen with AIR's Xpand!2). Loading it again is free.
void close(void*) {}
QString lastError() {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, GetLastError(), 0,
        (LPWSTR)&msg, 0, nullptr);
    QString s = msg ? QString::fromWCharArray(msg).trimmed() : QString("error %1").arg(GetLastError());
    LocalFree(msg);
    return s;
}
#else
void* open(const QString& path) { return dlopen(path.toLocal8Bit().constData(), RTLD_NOW | RTLD_LOCAL); }
void* symbol(void* lib, const char* name) { return lib ? dlsym(lib, name) : nullptr; }
void close(void* lib) {
    if (lib) dlclose(lib);
}
QString lastError() {
    const char* e = dlerror();
    return e ? QString::fromLocal8Bit(e) : QString();
}
#endif
}   // namespace mt::dynlib
