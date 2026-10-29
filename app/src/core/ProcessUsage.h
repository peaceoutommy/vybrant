// The memory this process holds, as the OS counts it (the transport bar's readout)
#pragma once

namespace mt {

double residentBytes();   // in RAM right now: Linux RSS, Windows working set

}   // namespace mt
