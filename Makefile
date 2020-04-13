PLUGIN = assert_introspect.so

all: $(PLUGIN)

$(PLUGIN): plugin.c
	g++ -g -Wall -Werror -I`gcc -print-file-name=plugin`/include -fpic -shared -o $@ $<

plugin_test.o: plugin_test.c $(PLUGIN)
	gcc -O2 -fplugin=./$(PLUGIN) plugin_test.c -c

tester: tester.c plugin_test.o
	gcc $^ -o $@

run: tester
	./tester

test: $(PLUGIN)
	cd tests && pytest -vv .

clean:
	rm -f $(PLUGIN) *.o

dump: plugin_test.o
	objdump -d plugin_test.o
