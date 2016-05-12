#include <stdlib.h>
#include <stdio.h>

void
swap(size_t v[], size_t i, size_t j)
{
	size_t tmp = v[i];
	v[i] = v[j];
	v[j] = tmp;
}

void
sort(size_t v[], int l, int r, int (*cmp)(size_t, size_t))
{
	if (l >= r)
		return;
	swap(v, l, (l + r) / 2);
	int last = l;
	for (size_t i = l + 1; i <= r; i++) {
		if (cmp(v[i], v[l]) > 0)
			swap(v, ++last, i);
	}
	swap(v, l, last);
	sort(v, l, last - 1, cmp);
	sort(v, last + 1, r, cmp);
}

size_t rand_numbers1[] = {
#       include "rand_numbers.txt"
};

size_t rand_numbers2[] = {
#       include "rand_numbers.txt"
};

int
cmp_gt(size_t a1, size_t a2)
{
	return a1 > a2;
}

int
cmp_lt(size_t a1, size_t a2)
{
	return a1 < a2;
}

#define LEN(x) (sizeof(x)/sizeof(x[0]))

int
main()
{
	printf("Sorting cmp1...\n");
	sort(rand_numbers1, 0, LEN(rand_numbers1) - 1, cmp_gt);
	for (size_t i = 0; i <  LEN(rand_numbers1); i++) {
		printf("%zu, ", rand_numbers1[i]);
	}
	printf("Sorting cmp2...\n");
	sort(rand_numbers2, 0, LEN(rand_numbers2) - 1, cmp_lt);
	for (size_t i = 0; i <  LEN(rand_numbers2); i++) {
		printf("%zu, ", rand_numbers2[i]);
	}
	return 0;
}



