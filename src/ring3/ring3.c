#include "ring3.h"
#include "../terminal/terminal.h"

void jump_to_ring3(void (*user_program)()) {
    (void)user_program;
    print("jump_to_ring3: paging is not enabled; refusing to leave ring 0\n");
}

void setup_user_stack(uint32_t stack_pointer) {
    (void)stack_pointer;
}

void setup_user_segments(void) {
}
