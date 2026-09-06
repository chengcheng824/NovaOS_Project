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

/* MinGW emits a __chkstk_ms call for stack frames > 4KB. The real one
 * probes pages downward (guard-page growth); our user stack is fully
 * backed RAM and the caller still does its own `sub %eax,%esp`, so a
 * no-op is correct. Defined as a C function AFTER nxp_entry: the first
 * .text byte is the program entry, and top-level __asm__ would be
 * hoisted ahead of every function (that bug shipped once already). */
void __chkstk_ms(void) { }
