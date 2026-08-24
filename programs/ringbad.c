#include "nxp.h"
void nxp_main(void)
{
    API->puts("[user] ringbad: writing kernel memory @0x100000...\n");
    *(volatile unsigned *)0x00100000u = 0x12345678u;  /* must page-fault */
    API->puts("SHOULD NOT GET HERE\n");
    API->exit();
}
