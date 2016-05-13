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

int func(int flag) {
  ft f = NULL;

  switch(flag) {
    case 1: f = f1; break;
    case 2: f = f2; break;
    default: f = f3; break;
  }

  int sum = 0;
  for (int i = 0; i < 1000000; i++) {
    sum += f(i);
  }

  return sum;
}

int main() {
  printf("%d\n", func(1));
  printf("%d\n", func(2));
  printf("%d\n", func(3));
  return 0;
}
