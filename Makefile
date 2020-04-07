PLUGIN = assert_introspection.so

all: $(PLUGIN)

$(PLUGIN): plugin.c
	g++ -g -Wall -Werror -I`gcc -print-file-name=plugin`/include -fpic -shared -o $@ $<

plugin_test.o: plugin_test.c $(PLUGIN)
	gcc -fplugin=./$(PLUGIN) plugin_test.c -c

tester: tester.c plugin_test.o
	gcc $^ -o $@

test: tester
	./tester

run: plugin_test.o

clean:
	rm -f $(PLUGIN) *.o

dump: plugin_test.o
	objdump -d plugin_test.o
