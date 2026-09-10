gain = hslider("gain",0.5,0,1,0.01);
process(x) = (sin(x) + cos(2*x)) * exp(-x*x) * gain;
