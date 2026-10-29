// Plugin libraries loaded the same way on every platform: dlopen on Linux, LoadLibrary on Windows
#pragma once
#include <QString>

namespace mt::dynlib {
void* open(const QString& path);   // nullptr on failure (lastError() says why)
void* symbol(void* lib, const char* name);
void close(void* lib);
QString lastError();
}   // namespace mt::dynlib
