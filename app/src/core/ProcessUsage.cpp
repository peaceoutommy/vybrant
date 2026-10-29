#include "core/ProcessUsage.h"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <cstdio>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace mt {

ProcessUsage processUsage() {
    ProcessUsage u;
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        auto secs = [](FILETIME f) {
            return (double)(((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7;
        };   // 100 ns units
        u.cpuSeconds = secs(kernel) + secs(user);
    }
    PROCESS_MEMORY_COUNTERS m{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof m)) u.residentBytes = (double)m.WorkingSetSize;
#else
    rusage r{};
    if (getrusage(RUSAGE_SELF, &r) == 0)
        u.cpuSeconds = r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
    long pages = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {   // size resident ... (in pages)
        if (std::fscanf(f, "%*ld %ld", &pages) != 1) pages = 0;
        std::fclose(f);
    }
    u.residentBytes = (double)pages * sysconf(_SC_PAGESIZE);
#endif
    return u;
}

}   // namespace mt
