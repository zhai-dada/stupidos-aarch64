#include "stupidos_user.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    u_puts((const int8_t *)"hello from user ELF program\n");
    return 0;
}
