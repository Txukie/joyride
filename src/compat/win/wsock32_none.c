/* joyride on Windows: no guest Winsock (GTA's network play is DirectPlay,
 * joyride's dplay.c); pcrecomp's wsock32.c is the BSD sockets' */
#include "win32hle.h"
const win32hle_shim win32hle_wsock32[] = { { NULL, NULL } };
void hle_ws_poll(void) {}       /* the pump's WSAAsyncSelect: no guest sockets */
