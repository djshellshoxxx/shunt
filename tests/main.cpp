#include "TestFramework.h"

int main(int argc, char** argv) {
    return tf::runAll(argc > 1 ? argv[1] : nullptr);
}
