TESTERS=$(wildcard *.c)
TESTS=$(TESTERS:%.c=%)

all: $(TESTS:%=run_%)
	echo $(TESTS)

%.ll: %.c
	clang -S $^ -O0 -emit-llvm

run_%: %.ll
	cd ../build && ninja
	../build/bin/osr_test_tool $^
