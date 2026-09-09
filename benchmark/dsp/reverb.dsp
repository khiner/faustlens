import("stdfaust.lib");
gain = hslider("gain",0.5,0,1,0.01);
process = re.mono_freeverb(0.7,0.5,0.1,44100) * gain;
