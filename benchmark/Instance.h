#pragma once

#include "Reference.h"
#include "runtime/Instance.h"

inline Reference ReferenceOf(faustlens::Instance *instance) {
    using faustlens::Instance;
    return {
        instance,
        [](void *p) { delete static_cast<Instance *>(p); },
        [](void *p, int rate) { static_cast<Instance *>(p)->Init(rate); },
        [](void *p, int n, double **in, double **out) { static_cast<Instance *>(p)->Compute(n, in, out); },
        [](void *p, double value) {
            auto &dsp = *static_cast<Instance *>(p);
            for (const auto &zone : dsp.Zones)
                if (zone.Kind == faustlens::UiKind::HSlider) dsp.SetControl(zone.Label, value);
        },
        instance->Inputs(),
        instance->Outputs()
    };
}
