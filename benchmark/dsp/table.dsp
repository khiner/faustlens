gain = hslider("gain",0.5,0,1,0.01);
process = (waveform{0.1,-0.2,0.3,-0.4},(abs(int(_ * 3)) % 4) : rdtable) * gain;
