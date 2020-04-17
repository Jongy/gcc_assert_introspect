import os.path

GCC = None
OUTPUT_FILE_PATH = os.path.join(os.path.dirname(__file__), "tests.log")
OUTPUT_FILE = None


def pytest_addoption(parser):
    parser.addoption(
        "--gcc",
        action="store",
        help="GCC executable to use",
        default="gcc",
    )


def pytest_configure(config):
    global GCC, OUTPUT_FILE
    GCC = config.option.gcc
    print("Using GCC {} in tests".format(GCC))
    print("Output is saved to {}".format(OUTPUT_FILE_PATH))
    OUTPUT_FILE = open(OUTPUT_FILE_PATH, "w")
