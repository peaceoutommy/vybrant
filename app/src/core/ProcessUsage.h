// This process's CPU time and memory, as the OS counts them (the title bar's usage readout)
#pragma once

namespace mt {

struct ProcessUsage {
    double cpuSeconds = 0;   // every thread, user + system, since the process started
    double residentBytes = 0;   // memory in RAM right now (Linux RSS, Windows working set)
};
ProcessUsage processUsage();

}   // namespace mt
