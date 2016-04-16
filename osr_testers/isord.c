#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int isord(long* v, long n, int (*c)(void* a, void* b)) {
    for (long i=1; i<n; i++)
        if (c(v+i-1,v+i)>0) return 0;
    return 1;
}

int c(void *a, void *b) {
  return 0;
}

int main() {
  long arr[5000];
  void* a = malloc(1); free(a);
  printf("running isord test\n");
  assert(isord(arr, 100,c) == 1);
  assert(isord(arr, 1001,c) == -1000);
  printf("passed\n");
}

