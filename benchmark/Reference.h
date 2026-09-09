#pragma once

struct Reference {
    void *Object;
    void (*Destroy)(void *);
    void (*Init)(void *, int);
    void (*Compute)(void *, int, double **, double **);
    void (*Control)(void *, double);
    int Inputs, Outputs;
};
