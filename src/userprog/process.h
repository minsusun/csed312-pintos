#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

/* Lab2 - userProcess */
typedef int pid_t;

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

/* Lab2 - userProcess */
void parse_filename (char *command);
int parse_arguments (char *command, char **argv);
void store_arguments (char **argv, int argc, void **esp);

#endif /* userprog/process.h */
