gain = hslider("gain",0.5,0,1,0.01);
process = ((+ : *(0.99)) ~ _) * gain;
