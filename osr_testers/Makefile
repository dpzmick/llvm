CC=../build/bin/clang
TESTERS=$(wildcard *.c)
TESTS=$(TESTERS:%.c=%)

all: $(TESTS:%=run_%)
	echo $(TESTS)

%.ll: %.c
	$(CC) -S $^ -O0 -emit-llvm

run_%: %.ll
	../build/bin/osr_test_tool $^

clean:
	rm *.ll *.bc
