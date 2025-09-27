#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>   // Required for the 'errno' variable
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "utils.h"

int main() {
    char* line = NULL;
    size_t len = 0;
    ssize_t read;

    // The main shell loop
    while (1) {
        // 1. Display prompt
        printf("$ ");
        fflush(stdout); // Ensure prompt is displayed immediately

        // 2. Read a line of input
        read = getline(&line, &len, stdin);
        if (read == -1) {
            break; // Exit on EOF (Ctrl+D)
        }

        // If user just hits enter, continue to next loop iteration
        if (strcmp(line, "\n") == 0) {
            continue;
        }


        // 3. Parse the input into an array of arguments
        char** args = parse_command(line);

        if (args[0] != NULL && strcmp(args[0], "exit") == 0) {
            break;
        }

        Redirs R;
        char *errmsg = NULL;
        int signal = parse_redirs(args, &R, &errmsg);
        if (signal == -1) {
            printf("%s\n", errmsg);
            continue;
        }

        pid_t pid = fork();
        if (pid == 0) {
            int fd;
            if (R.in_file) {
                fd = open(R.in_file, O_RDONLY);
                if (fd < 0) { perror(R.in_file); _exit(1); }
                if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2 <"); _exit(1); }
                close(fd);
            }
            if (R.out_file) {
                fd = open(R.out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) { perror(R.out_file); _exit(1); }
                if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2 >"); _exit(1); }
                close(fd);
            }
            if (R.err_file) {
                fd = open(R.err_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) { perror(R.err_file); _exit(1); }
                if (dup2(fd, STDERR_FILENO) < 0) { perror("dup2 2>"); _exit(1); }
                close(fd);
            }
            
            // if args[0] is valid 
            if (execvp(args[0], args) == -1) {
                // Check if the specific error is "No such file or directory"
                if (errno == ENOENT) {
                    fprintf(stderr, "%s: command not found\n", args[0]);
                }
                else {
                    // For all other errors, print the default system error
                    fprintf(stderr, "%s: %s\n", args[0], strerror(errno));
                }
                exit(EXIT_FAILURE); // Crucial: exit the child process
            }
        }
        
        else if (pid > 0) {
            // --- This is the PARENT process ---
            // 6. Wait for the child process to finish 
            wait(NULL);

        }
        
        else {
            // Forking failed
            perror("fork");
        }

        // 7. Free the allocated memory for arguments
        free(args);
    }

    // Final cleanup
    free(line);
    return 0;
}