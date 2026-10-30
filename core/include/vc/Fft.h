#pragma once
#include <cmath>
#include <complex>
#include <utility>
#include <vector>

namespace vc {

// Radix-2 complex FFT. Forward is unnormalised, inverse divides by n.
class Fft {
public:
    using cf = std::complex<float>;

    explicit Fft(int n) : n_(n), tw_(static_cast<size_t>(n / 2)), rev_(static_cast<size_t>(n))
    {
        int lg = 0;
        while ((1 << lg) < n) ++lg;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < lg; ++b)
                if (i & (1 << b)) r |= 1 << (lg - 1 - b);
            rev_[static_cast<size_t>(i)] = r;
        }
        for (int i = 0; i < n / 2; ++i)
            tw_[static_cast<size_t>(i)] = std::polar(1.0f, -6.28318530717958647692f * static_cast<float>(i) / static_cast<float>(n));
    }

    int size() const { return n_; }
    void forward(cf* x) const { transform(x, false); }
    void inverse(cf* x) const
    {
        transform(x, true);
        const float s = 1.0f / static_cast<float>(n_);
        for (int i = 0; i < n_; ++i) x[i] *= s;
    }

private:
    void transform(cf* x, bool inv) const
    {
        for (int i = 0; i < n_; ++i) {
            const int j = rev_[static_cast<size_t>(i)];
            if (i < j) std::swap(x[i], x[j]);
        }
        for (int len = 2; len <= n_; len <<= 1) {
            const int half = len >> 1, step = n_ / len;
            for (int i = 0; i < n_; i += len) {
                for (int j = 0; j < half; ++j) {
                    const cf w0 = tw_[static_cast<size_t>(j * step)];
                    const float wr = w0.real(), wi = inv ? -w0.imag() : w0.imag();
                    cf& a = x[i + j];
                    cf& b = x[i + j + half];
                    const cf t(b.real() * wr - b.imag() * wi, b.real() * wi + b.imag() * wr);
                    b = a - t;
                    a = a + t;
                }
            }
        }
    }

    int n_;
    std::vector<cf> tw_;
    std::vector<int> rev_;
};

} // namespace vc
