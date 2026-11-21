/*
 * joyride - ddraw.dll exports win32hle's DirectDraw does not answer.
 *
 * SciTech MGL looks up DirectDrawCreate and DirectDrawCreateClipper with
 * GetProcAddress and only registers its window class (MGLDirectDrawWindow,
 * whose window procedure feeds MGL's keyboard and mouse events) if both
 * exist. Without the clipper export the game draws but never sees input.
 *
 * Also replaces IDirectDrawSurface::Flip (see x_Flip). Vtables are built by
 * resolving method names, and the first registration of a name wins.
 */
#include <string.h>

#include "win32hle.h"
#include "ddraw_int.h"
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

/* IDirectDrawSurface::Flip(this, target, flags): copy the back buffer to the
 * front instead of swapping their memory.
 *
 * win32hle swaps the two surfaces' pixel pointers, as real DirectDraw swaps
 * video memory. MGL, though, keeps drawing into the back buffer address it
 * got once: after a swap that address is the front buffer, so every other
 * frame went to the screen while the presenter showed the frame before.
 * With the camera moving (it climbs and drops with the player's speed) the
 * picture flickered between two heights: "the camera zooms in and out".
 * Copying keeps each surface's memory where the game expects it. */
static void x_Flip(void) {
    if (hle_dd_access_hook) hle_dd_access_hook();
    surf_t *s = hle_dd_surf(A32(0));
    if (s && s->back && s->back->px && s->px) {
        surf_t *b = s->back;
        int rows = s->h < b->h ? s->h : b->h;
        int bytes = (s->pitch < b->pitch ? s->pitch : b->pitch);
        if (s->pitch == b->pitch) memcpy(s->px, b->px, (size_t)s->pitch * (size_t)rows);
        else for (int y = 0; y < rows; y++)
            memcpy(s->px + (size_t)y * s->pitch, b->px + (size_t)y * b->pitch, (size_t)bytes);
        joyride_flipping = 1;                                   /* the presenter shows (and paces) each flip */
        hle_dd_mark(s);
        joyride_flipping = 0;
    }
    RET(0, 3);                                                   /* DD_OK */
}

const win32hle_shim joyride_ddraw[] = {
    { "DirectDrawCreateClipper", x_DirectDrawCreateClipper },
    { "IDirectDrawSurface::Flip", x_Flip },
    { NULL, NULL },
};
