GCC = None

def pytest_addoption(parser):
    parser.addoption(
        "--gcc",
        action="store",
        help="GCC executable to use",
        default="gcc",
    )


def pytest_configure(config):
    global GCC
    GCC = config.option.gcc
    print("Using GCC {} in tests".format(GCC))
