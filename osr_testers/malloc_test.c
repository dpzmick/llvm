#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

void* loopy(size_t n) {
  if (n < 100) return NULL;

  size_t i = 0;
  size_t iterations = 0;
  void* thing;

  for (i = 0; i < n; i++) {
    iterations++;
    void* thing1 = malloc(sizeof(int));
    if (rand() == 0) {
      thing = thing1;
    }
  }

  if (rand() != 0) {
    thing = loopy(n/100);
  }

  return thing;
}

int main() {
  loopy(10000);
  loopy(10000);
  return 0;
}
