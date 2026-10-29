#include "core/ProcessUsage.h"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace mt {

double residentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS m{};
    return GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof m) ? (double)m.WorkingSetSize : 0;
#else
    long pages = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {   // size resident ... (in pages)
        if (std::fscanf(f, "%*ld %ld", &pages) != 1) pages = 0;
        std::fclose(f);
    }
    return (double)pages * sysconf(_SC_PAGESIZE);
#endif
}

}   // namespace mt
