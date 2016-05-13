#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int isord(long* v, long n, int (*c)(void* a, void* b)) {
  for (long i=1; i<n; i++)
    if (!c(v+i-1,v + i)) return 0;

  return 1;
}

int comp2(void *a, void *b) {
  long ia = *(long*)a;
  long ib = *(long*)b;

  return ia <= ib;
}

int main() {
  size_t max = 1000000;
  long *arr = malloc(sizeof(long) * max);
  memset(arr, 0, max*sizeof(long));

  printf("running isord test\n");

  assert(isord(arr, max, comp2) == 1); // should osr
  printf("passed\n");

  free(arr);
}

