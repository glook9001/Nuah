#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

int main(int argc, char** argv) {
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        perror("readlink");
        return 1;
    }
    exe_path[len] = '\0';

    char* slash = strrchr(exe_path, '/');
    if (slash) {
        *slash = '\0';
    }

    setenv("APPDIR", exe_path, 1);

    char script_path[PATH_MAX];
    snprintf(script_path, sizeof(script_path), "%s/nuah-apprun.sh", exe_path);

    char** new_argv = malloc((argc + 2) * sizeof(char*));
    new_argv[0] = "/bin/bash";
    new_argv[1] = script_path;
    for (int i = 1; i < argc; i++) {
        new_argv[i + 1] = argv[i];
    }
    new_argv[argc + 1] = NULL;

    execv("/bin/bash", new_argv);
    perror("execv /bin/bash");
    return 1;
}
