#include "disk.h"
#include "fs.h"
#include "shell.h"

int main(int argc, char **argv) {
    FS f;

    f.format();
    //f.create("some\n\nhello");

    //f.ls();

    //f.cat("some");

    Shell shell;
    shell.run();
    return 0;
}
