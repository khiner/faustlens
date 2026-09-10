gain = hslider("gain",0.5,0,1,0.01);
process(x) = pow(abs(x)+0.25, 1.5+0.25*sin(x)) * gain;
