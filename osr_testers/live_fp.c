#include <stdlib.h>
#include <stdio.h>

typedef int (*ft)(int);

int f1(int a) { return a; }
int f2(int a) { return a + 1; }
int f3(int a) { return -12; }

int func(int flag) {
  ft f = NULL;

  switch(flag) {
    case 1: f = f1;
    case 2: f = f2;
    default: f = f3;
  }

  int sum = 0;
  for (int i = 0; i < 1200; i++) {
    sum += f(i);
  }

  return sum;
}

int main() {
  func(1);
  func(2);
  func(3);
  return 0;
}
