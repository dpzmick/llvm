#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

typedef int (*ft)(size_t);

int f1(size_t a) { return a; }
int f2(size_t a) { return a + 1; }
int f3(size_t a) { return -12; }

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
  assert(2*1001 == loopy(f1, 1001));
  assert(2*1001 == loopy(f2, 1001));
  assert(2*1001 == loopy(f3, 1001));
  return 0;
}
