gain = hslider("gain",0.5,0,1,0.01);
process(x) = sum(i,16, sin(x*float(i+1))/float(i+1)) * gain;
