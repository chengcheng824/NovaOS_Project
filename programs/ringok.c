#include "nxp.h"
void nxp_main(void)
{
    API->puts("[user] ring3 alive (ringok)\n");
    API->exit();
}
