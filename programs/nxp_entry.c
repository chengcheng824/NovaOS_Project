/* entry stub: linked FIRST, lands at the .nxp entry point.
 * kernel enters with EAX = API table; stash it and call nxp_main(). */
#include "nxp.h"

nxp_api_t *nxp_api_ptr;

void nxp_entry(void)
{
    /* MinGW: reference the global by its asm name; EAX survives the
     * function prologue at -O0 */
    __asm__ volatile ("movl %%eax, _nxp_api_ptr" : : : "memory");
    nxp_main();
}
