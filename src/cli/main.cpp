#include <iostream>
#include <CLI/CLI.hpp>
#include <peersync/peersync.hpp>

int main(int argc, char* argv[]) {
    CLI::App app{"peersync CLI (placeholder)"};
    CLI11_PARSE(app, argc, argv);
    std::cout << "peersync CLI (placeholder)\n";
    return 0;
}
