#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int isord(long* v, long n, int (*c)(void* a, void* b)) {
  for (long i=1; i<n; i++)
    if (c(v+i-1,v+i)>0) return 0;

  return 1;
}

int comp1(void *a, void *b) {
  return 0;
}

int comp2(void *a, void *b) {
  return 0;
}

int main() {
  size_t max = 1000000;
  long arr[max];
  void* a = malloc(1); free(a);
  printf("running isord test\n");

  assert(isord(arr, 100, comp1) == 1);
  assert(isord(arr, max, comp1) == 1);
  assert(isord(arr, max, comp2) == 1);
  printf("passed\n");
}

