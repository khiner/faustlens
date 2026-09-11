gain = hslider("gain", 0.5, 0, 1, 0.01);
index = (+(1) ~ _) % 64;
process(x) = rwtable(64, 0.0, index, gain*x, (index+32)%64);
