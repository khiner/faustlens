#include "runtime/Interp.h"
#ifdef BENCH_NATIVE
#include "runtime/Native.h"
#endif

int main() {
    faustlens::Plan p;
    faustlens::UiNode ui;
    faustlens::Interp interpreter(p, ui);
    interpreter.Init(48000);
#ifdef BENCH_NATIVE
    auto native = faustlens::Native::Compile(p, ui);
    if (!native) return 1;
    (*native)->Init(48000);
#endif
}
