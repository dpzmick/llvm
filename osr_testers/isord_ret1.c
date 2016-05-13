#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int isord(long* v, long n, int (*c)(void* a, void* b)) {
  for (long i=1; i<n; i++)
    if (!c(v+i-1,v + i)) return 0;

  return 1;
}

int comp1(void *a, void *b) {
  return 1;
}

int main() {
  size_t max = 1000000;
  long *arr = malloc(sizeof(long) * max);
  memset(arr, 0, max*sizeof(long));

  void* a = malloc(1); free(a);
  printf("running isord test\n");

  assert(isord(arr, 100, comp1) == 1); // should not do osr
  assert(isord(arr, max, comp1) == 1); // should osr and comp -> ret 1
  printf("passed\n");

  free(arr);
}
