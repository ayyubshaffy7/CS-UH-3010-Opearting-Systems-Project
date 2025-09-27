#ifndef UTILS_H
#define UTILS_H

typedef struct {
    char *in_file;   // for <
    char *out_file;  // for >
    char *err_file;  // for 2>
} Redirs;

char** parse_command(char* input);  // function to parse a command string into an array of arguments
int parse_redirs(char **args, Redirs *R, char **errmsg);  // scan args to extract redirections and compact argv


#endif //UTILS_H