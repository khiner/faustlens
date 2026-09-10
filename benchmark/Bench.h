#pragma once

#include "Reference.h"

#include <algorithm>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#endif

using Clock = std::chrono::steady_clock;

inline void PrepareThread() {
    std::fesetenv(FE_DFL_ENV);
#if defined(__APPLE__)
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0)) throw std::runtime_error("cannot set benchmark thread QoS");
#endif
}

inline double Ms(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
inline double Quantile(std::vector<double> values, double q) {
    std::ranges::sort(values);
    return values[std::min(values.size() - 1, size_t(std::ceil(q * double(values.size()))) - 1)];
}

struct ReferenceOwner {
    Reference Dsp;
    ~ReferenceOwner() { Dsp.Destroy(Dsp.Object); }
    ReferenceOwner(Reference dsp) : Dsp(dsp) {}
    ReferenceOwner(const ReferenceOwner &) = delete;
    ReferenceOwner &operator=(const ReferenceOwner &) = delete;
};

struct Buffers {
    std::vector<std::vector<double>> In, Out;
    std::vector<double *> Ip, Op;
    Buffers(int inputs, int outputs, int frames, double offset = 0) : In(inputs, std::vector<double>(frames)), Out(outputs, std::vector<double>(frames)) {
        for (int c = 0; c < inputs; ++c) {
            for (int j = 0; j < frames; ++j) In[c][j] = offset + 0.125 * std::sin(0.017 * j + 0.13 * c) + 0.125 * (double(j % 17) / 16 - 0.5);
            Ip.push_back(In[c].data());
        }
        for (auto &channel : Out) Op.push_back(channel.data());
    }
};

struct Accuracy {
    double Max = 0, Squared = 0, Peak = 0;
    size_t Compared = 0, Nonfinite = 0;
    bool Ok() const { return !Nonfinite && Compared && Max <= 1e-10 * std::max(1.0, Peak); }
    double Rms() const { return std::sqrt(Squared / std::max<size_t>(Compared, 1)); }
};

inline Accuracy Compare(Reference dsp, Reference ref, int block) {
    Buffers data(dsp.Inputs, dsp.Outputs, block), oracle(ref.Inputs, ref.Outputs, block);
    for (auto r : {dsp, ref}) {
        r.Init(r.Object, 48000);
        r.Control(r.Object, 0.7);
    }
    Accuracy result;
    for (int b = 0; b < (48000 + block - 1) / block; ++b) {
        if (b == 97)
            for (auto r : {dsp, ref}) r.Control(r.Object, 0.3);
        dsp.Compute(dsp.Object, block, data.Ip.data(), data.Op.data());
        ref.Compute(ref.Object, block, oracle.Ip.data(), oracle.Op.data());
        for (int c = 0; c < ref.Outputs; ++c)
            for (int j = 0; j < block; ++j) {
                const double a = data.Out[c][j], b = oracle.Out[c][j];
                if (!std::isfinite(a) || !std::isfinite(b)) {
                    ++result.Nonfinite;
                    continue;
                }
                const double error = std::abs(a - b);
                result.Max = std::max(result.Max, error);
                result.Squared += error * error;
                result.Peak = std::max(result.Peak, std::abs(b));
                ++result.Compared;
            }
    }
    return result;
}

struct Rendering {
    std::vector<double> NsPerFrame;
    double Checksum = 0;
};

inline Rendering Render(Reference dsp, int block, double inputOffset = 0) {
    Buffers data(dsp.Inputs, dsp.Outputs, block, inputOffset);
    Rendering result;
    const auto compute = [&] { dsp.Compute(dsp.Object, block, data.Ip.data(), data.Op.data()); };
    const int blocks = std::max(32, (65536 + block - 1) / block);
    dsp.Init(dsp.Object, 48000);
    dsp.Control(dsp.Object, 0.7);
    const auto warm = Clock::now() + std::chrono::milliseconds(100);
    do {
        for (int i = 0; i < blocks; ++i) compute();
    } while (Clock::now() < warm);
    for (int run = 0; run < 9; ++run) {
        dsp.Init(dsp.Object, 48000);
        dsp.Control(dsp.Object, 0.7);
        for (int warm = 0; warm < 32; ++warm) compute();
        const auto start = Clock::now();
        for (int i = 0; i < blocks; ++i) compute();
        result.NsPerFrame.push_back(Ms(start) * 1e6 / (blocks * block));
        for (const auto &channel : data.Out) result.Checksum += std::accumulate(channel.begin(), channel.end(), 0.0);
    }
    return result;
}
