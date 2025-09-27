#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utils.h"

#define TOKEN_DELIMITERS " \n" // space and newline are the only delimiters

/**
 * Parses the input line into a NULL-terminated array of tokens (arguments).
 * @param input The raw command line string from the user.
 * @return A dynamically allocated array of strings. The last element is NULL.
 */

char** parse_command(char* input) {

    int bufsize = 64; // initial array size for the list of arguments
    int position = 0;

    // Allocate memory for an array of character pointers (strings)
    char** tokens = malloc(bufsize * sizeof(char*));
    char* token;

    if (!tokens) {
        fprintf(stderr, "myshell: allocation error\n");
        exit(EXIT_FAILURE);
    }

    // Use strtok to split the input string into tokens
    token = strtok(input, TOKEN_DELIMITERS);

    while (token != NULL) {
        tokens[position] = token;
        position++;

        // If we've exceeded the buffer, reallocate more memory
        if (position >= bufsize) {
            bufsize += 64;
            tokens = realloc(tokens, bufsize * sizeof(char*));
            if (!tokens) {
                fprintf(stderr, "myshell: allocation error\n");
                exit(EXIT_FAILURE);
            }
        }

        token = strtok(NULL, TOKEN_DELIMITERS);
    }

    // The last element of the array must be NULL for execvp
    tokens[position] = NULL;
    return tokens;
}

int parse_redirs(char **args, Redirs *R, char **errmsg) {
    R->in_file = R->out_file = R->err_file = NULL;

    int write_idx = 0;
    for (int i = 0; args[i] != NULL; i++) {
        if (strcmp(args[i], "<") == 0) {
            if (R->in_file) { *errmsg = "Duplicate input redirection."; return -1; }
            if (!args[i+1]) { *errmsg = "Input file not specified."; return -1; }
            R->in_file = args[i+1];
            i++; // skip filename
            continue;
        }
        if (strcmp(args[i], ">") == 0) {
            if (R->out_file) { *errmsg = "Duplicate output redirection."; return -1; }
            if (!args[i+1]) { *errmsg = "Output file not specified."; return -1; }
            R->out_file = args[i+1];
            i++;
            continue;
        }
        if (strcmp(args[i], "2>") == 0) {
            if (R->err_file) { *errmsg = "Duplicate error redirection."; return -1; }
            if (!args[i+1]) { *errmsg = "Error output file not specified."; return -1; }
            R->err_file = args[i+1];
            i++;
            continue;
        }
        // keep normal argv tokens
        args[write_idx++] = args[i];
    }
    args[write_idx] = NULL;

    if (!args[0]) { *errmsg = "Command missing."; return -1; }
    return 0;
}