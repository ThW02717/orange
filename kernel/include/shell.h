#ifndef SHELLF_H
#define SHELLF_H
#include <stdint.h>

typedef struct Shell{
    int32_t pid;
    char* command;
}shell_t;


void processCommand(shell_t* shell);
void runAShell(int32_t pid);
void shell_set_context(unsigned long hartid, unsigned long dtb_addr,
                       uint64_t initrd_start_hint, uint64_t initrd_end_hint);
void shell_redraw_prompt_delayed(void);
int shell_load_user_program_named(const char *name, uintptr_t *entry_out, unsigned long *size_out);

#endif
