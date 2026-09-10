gain = hslider("gain",0.5,0,1,0.01);
process(x) = sin(3*x) * gain;
