#pragma once
#include <algorithm>
#include <cmath>

namespace vc {

// Transposed direct form II. setNotch() is an RBJ peaking EQ with negative gain,
// so a cut of 0 dB is an exact identity.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;

    void setNotch(double fs, double freq, double q, double cutDb)
    {
        const double f = std::min(freq, 0.49 * fs);
        const double A = std::pow(10.0, -cutDb / 40.0);
        const double w0 = 6.283185307179586 * f / fs;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double c = std::cos(w0);
        const double a0 = 1.0 + alpha / A;
        b0 = static_cast<float>((1.0 + alpha * A) / a0);
        b1 = static_cast<float>(-2.0 * c / a0);
        b2 = static_cast<float>((1.0 - alpha * A) / a0);
        a1 = b1;
        a2 = static_cast<float>((1.0 - alpha / A) / a0);
    }

    void clear() { z1 = z2 = 0; }

    float process(float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

} // namespace vc
