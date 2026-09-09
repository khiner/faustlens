import("stdfaust.lib");
gain = hslider("gain",0.5,0,1,0.01);
process = seq(i,16,fi.lowpass(2,1000+i*100)) * gain;
