import("stdfaust.lib");
process = (_ * hslider("gain",0.5,0,1,0.01) : mem), sin(_),
    ffunction(float probe(float), "host.h", ""),
    fconstant(int fSamplingFreq, "math.h"), fvariable(int count, "math.h"),
    (0, 0 : soundfile("missing", 1));
