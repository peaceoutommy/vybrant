// Radix-2 FFT for analysis (spectrum displays). Allocation-free once built, so it can run on the audio thread; sizes are
// powers of two. Our own rather than FFTW, whose GPL would bind the whole app.
#pragma once
#include <complex>
#include <vector>

namespace mt::dsp {

class Fft {
public:
    explicit Fft(int n);   // n: a power of two
    int size() const { return m_n; }
    // size() real samples in; bins 0 .. size()/2 (DC .. Nyquist) out, unscaled (a full-scale sine reads about size()/2)
    void forwardReal(const float* in, std::complex<float>* out);

private:
    int m_n;
    std::vector<std::complex<float>> m_twiddle, m_buf;   // e^(-2 pi i k / n) for k < n/2; the working buffer
    std::vector<int> m_bitrev;
};

}   // namespace mt::dsp
