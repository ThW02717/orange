#ifndef USER_H
#define USER_H

#include <stdint.h>

void enter_user_mode(void);

void user_mark_exit(void);
int user_has_exited(void);
void user_return_to_shell(void);

#endif
