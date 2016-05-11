#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#define ITERS 500

typedef int (*ft)(size_t);

int f1(size_t a) {
  int f = 0;

  for (size_t i = 0; i < ITERS; i++) {
    f += a;
  }

  return f;
}

int f2(size_t a) {
  int f = 0;

  for (size_t i = 0; i < ITERS; i++) {
    f += a - 1;
  }

  return f;
}

int f3(size_t a) {
  int f = 0;

  for (size_t i = 0; i < ITERS; i++) {
    f += -12;
  }

  return f;
}

int loopy(ft f, size_t n) {
  size_t i = 0;
  size_t iterations = 0;
  int sum = 0;

  for (i = 0; i < n; i++) {
    iterations++;
    sum += f(i);
  }

  for (i = 0; i < n; i++) {
    iterations++;
    sum += f(i);
  }

  return iterations;
}

int main() {
  int n = 1000000;
  assert(2*n == loopy(f1, n));
  assert(2*n == loopy(f2, n));
  assert(2*n == loopy(f3, n));
  return 0;
}
