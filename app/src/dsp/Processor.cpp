#include "dsp/Processor.h"
#include <QJsonArray>

namespace mt {

QJsonObject IProcessor::saveState() const {
    QJsonObject o;
    QJsonObject p;
    auto& ps = const_cast<IProcessor*>(this)->params();
    for (int i = 0; i < ps.count(); ++i) p[QString::number(ps.desc(i).id)] = ps.getAt(i);
    o["params"] = p;
    o["key"] = key();
    return o;
}

void IProcessor::restoreState(const QJsonObject& s) {
    auto p = s["params"].toObject();
    for (auto it = p.begin(); it != p.end(); ++it) params().set(it.key().toUInt(), (float)it.value().toDouble());
}

std::map<QString, NativeEditorMaker>& nativeEditors() {
    static std::map<QString, NativeEditorMaker> m;
    return m;
}
bool NativeProcessor::hasCustomEditor() const { return nativeEditors().count(key()) > 0; }
QWidget* NativeProcessor::createEditor(QWidget* parent) {
    auto it = nativeEditors().find(key());
    return it == nativeEditors().end() ? nullptr : it->second(this, parent);
}

QString IProcessor::formatValue(uint32_t id) const {
    auto& ps = const_cast<IProcessor*>(this)->params();
    const ParamDesc* d = ps.descById(id);
    if (!d) return QString();
    float v = ps.get(id);
    switch (d->unit) {
    case ParamUnit::Decibel: return QString::number(v, 'f', 1) + " dB";
    case ParamUnit::Hertz: return v >= 1000 ? QString::number(v / 1000, 'f', 2) + " kHz" : QString::number(v, 'f', 1) + " Hz";
    case ParamUnit::Milliseconds: return QString::number(v, 'f', 1) + " ms";
    case ParamUnit::Seconds: return v < 1 ? QString::number(v * 1000, 'f', 0) + " ms" : QString::number(v, 'f', 2) + " s";
    case ParamUnit::Percent: return QString::number(v * 100, 'f', 0) + " %";
    case ParamUnit::Semitones: return QString::number(v, 'f', d->steps ? 0 : 1) + " st";
    case ParamUnit::Cents: return QString::number(v, 'f', 0) + " ct";
    case ParamUnit::Ratio: return QString::number(v, 'f', 2) + ":1";
    case ParamUnit::Degrees: return QString::number(v, 'f', 0) + "\u00b0";
    case ParamUnit::Choice: {
        int i = (int)std::lround(v);
        if (i >= 0 && i < (int)d->choices.size()) return QString::fromStdString(d->choices[i]);
        return QString::number(i);
    }
    case ParamUnit::Toggle: return v >= 0.5f ? "On" : "Off";
    case ParamUnit::Bpm: return QString::number(v, 'f', 1) + " BPM";
    case ParamUnit::Multiplier: return QString::number(v, 'f', 2) + " x";
    default: return QString::number(v, 'f', d->steps ? 0 : 2);
    }
}

}   // namespace mt
