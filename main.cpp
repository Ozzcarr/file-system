#include "disk.h"
#include "fs.h"
#include "shell.h"

int main(int argc, char **argv) {
    FS f;
    Shell shell;
    shell.run();
    return 0;
}
