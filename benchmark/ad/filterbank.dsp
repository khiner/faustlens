// Sum sixteen independent poles driven by one input.
pole(i) = (+ : *(a)) ~ _ with {
    a = hslider("pole %i", 0.1 + 0.04*i, 0.01, 0.9, 0.001);
};
process(x) = sum(i, 16, x : pole(i));
