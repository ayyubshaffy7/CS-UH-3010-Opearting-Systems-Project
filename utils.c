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
