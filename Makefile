GCC ?= gcc
GXX ?= g++
PLUGIN = assert_introspect.so

all: $(PLUGIN)

$(PLUGIN): plugin.c
	$(GXX) -g -Wall -Werror -I`$(GCC) -print-file-name=plugin`/include -fpic -shared -o $@ $<

plugin_test.o: plugin_test.c $(PLUGIN)
	$(GCC) -Wall -Werror -O2 -fplugin=./$(PLUGIN) plugin_test.c -c

tester: tester.c plugin_test.o
	$(GCC) $^ -o $@

run: tester
	./tester

test: $(PLUGIN)
	cd tests && pytest -vv --gcc $(GCC) .

clean:
	rm -f $(PLUGIN) *.o

dump: plugin_test.o
	objdump -d plugin_test.o
