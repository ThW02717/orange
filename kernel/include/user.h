#ifndef USER_H
#define USER_H

#include <stdint.h>

void enter_user_mode(void);
void user_task_entry(void *arg);

void user_reset_trapframe(struct thread *task);
void user_mark_exit(void);
int user_has_exited(void);
void user_schedule_after_exit(void);

#endif
