#include "dsp/Fft.h"
#include <cassert>
#include <cmath>

namespace mt::dsp {

Fft::Fft(int n)
    : m_n(n)
    , m_twiddle(n / 2)
    , m_buf(n)
    , m_bitrev(n) {
    assert(n >= 2 && (n & (n - 1)) == 0);
    const double tau = 6.283185307179586476925;
    for (int k = 0; k < n / 2; ++k) m_twiddle[k] = std::complex<float>(std::polar(1.0, -tau * k / n));
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i >> b & 1) r |= 1 << (bits - 1 - b);
        m_bitrev[i] = r;
    }
}

void Fft::forwardReal(const float* in, std::complex<float>* out) {
    for (int i = 0; i < m_n; ++i) m_buf[m_bitrev[i]] = in[i];
    for (int len = 2; len <= m_n; len <<= 1) {   // iterative Cooley-Tukey: butterflies over ever longer spans
        const int half = len / 2, step = m_n / len;
        for (int s = 0; s < m_n; s += len)
            for (int k = 0; k < half; ++k) {
                const std::complex<float> t = m_twiddle[k * step] * m_buf[s + k + half];
                m_buf[s + k + half] = m_buf[s + k] - t;
                m_buf[s + k] += t;
            }
    }
    for (int k = 0; k <= m_n / 2; ++k) out[k] = m_buf[k];
}

}   // namespace mt::dsp
