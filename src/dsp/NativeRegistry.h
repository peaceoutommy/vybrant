// Factory for native instruments & effects, keyed by stable string keys stored in the project.
#pragma once
#include "dsp/Processor.h"
#include <memory>
#include <vector>
#include <functional>

namespace mt {

struct NativeProcessorInfo { QString key; QString name; QString category; ProcessorKind kind; std::function<std::unique_ptr<IProcessor>()> factory; };

class NativeRegistry {
public:
    static NativeRegistry& instance();
    const std::vector<NativeProcessorInfo>& all() const { return m_infos; }
    std::unique_ptr<IProcessor> create(const QString& key) const;
    const NativeProcessorInfo* info(const QString& key) const;
    std::vector<const NativeProcessorInfo*> byKind(ProcessorKind k) const;
private:
    NativeRegistry();
    std::vector<NativeProcessorInfo> m_infos;
};

} // namespace mt
