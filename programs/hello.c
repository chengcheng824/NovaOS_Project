/* hello.nxp - example user program compiled on the host.
 * Build: just run build.ps1; it compiles programs\*.c (except
 * nxp_entry.c) and injects them into the NovaFS image. */
#include "nxp.h"

void nxp_main(void)
{
    API->puts("hello.nxp: compiled on the host with gcc!\n");

    if (API->scr_w) {
        API->text(100, 260, "16 bars, drawn by a host-compiled program", 0x00FFFFFF);
        for (int i = 0; i < 16; i++) {
            u32 r = (u32)(i * 16);
            u32 b = (u32)(255 - i * 16);
            u32 rgb = (r << 16) | b;
            API->fill_rect(100 + i * 40, 300, 24, 120, rgb);
        }
    }

    API->puts("press any key to exit...\n");
    while (API->getchar() < 0) { }      /* getchar is non-blocking now */
    API->exit();
}
