gain = hslider("gain",0.5,0,1,0.01);
process = (_ @ 1024) * gain;
