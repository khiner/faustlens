import("stdfaust.lib");
gain = hslider("gain",0.5,0,1,0.01);
process = os.osc(440) * gain;
