/*
 * joyride - ddraw.dll exports win32hle's DirectDraw does not answer.
 *
 * SciTech MGL looks up DirectDrawCreate and DirectDrawCreateClipper with
 * GetProcAddress and only registers its window class (MGLDirectDrawWindow,
 * whose window procedure feeds MGL's keyboard and mouse events) if both
 * exist. Without the clipper export the game draws but never sees input.
 */
#include "win32hle.h"
#include "joyride_hle.h"

/* (flags, LPDIRECTDRAWCLIPPER *out, IUnknown *outer) -> HRESULT
 * The same as IDirectDraw::CreateClipper(this, flags, out, outer), whose
 * win32hle implementation does not use `this`. */
static void x_DirectDrawCreateClipper(void) {
    uint32_t create = hle_resolve("IDirectDraw::CreateClipper");
    if (!create || !A32(1)) RET(0x80004005u, 3);               /* E_FAIL */
    uint32_t args[4] = { 0, A32(0), A32(1), A32(2) };
    uint32_t hr = hle_call_guest(create, 4, args);
    RET(hr, 3);
}

const win32hle_shim joyride_ddraw[] = {
    { "DirectDrawCreateClipper", x_DirectDrawCreateClipper },
    { NULL, NULL },
};
