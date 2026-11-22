/*
 * joyride - the texture filter's per-pixel part, compiled into the lifted
 * span loops themselves (CMake force-includes this into the generated C,
 * after recomp_funcs.h). tools/postlift.py splices joyride_tex_record into
 * each hooked texel fetch; see src/hle/texfilter.c for the whole story.
 *
 * Inline, a pixel costs a few loads and two stores; the slot lookups only
 * leave the loop when the page or palette differs from the last pixel's
 * (texfilter.c resets the "last" ones at each frame, so a frame's first use
 * of each goes through the slow path and marks it used).
 */
#ifndef JOYRIDE_TEXFILTER_INLINE_H
#define JOYRIDE_TEXFILTER_INLINE_H

#include <stdint.h>

/* guest memory, without the runtime's macros (included after this) */
#define JR_G8(a)  (*(const volatile uint8_t *)(uintptr_t)(RECOMP_MEM_BASE + (uint32_t)(a)))
#define JR_G32(a) (*(const volatile uint32_t *)(uintptr_t)(RECOMP_MEM_BASE + (uint32_t)(a)))

extern uint32_t joyride_rows;          /* the renderer's row table (display.c) */
extern uint32_t joyride_texfilter, joyride_texc, joyride_texclamp;
extern uint32_t *joyride_gb, joyride_gb_n;
extern int joyride_gb_used;
extern uint32_t joyride_gb_last_page, joyride_gb_last_pal, joyride_gb_last_page_slot, joyride_gb_last_pal_slot;
uint32_t joyride_bilerp32(uint32_t addr, uint32_t fu, uint32_t fv, uint32_t pal, uint32_t transparent);
uint32_t joyride_tex_page_slot(uint32_t page);
uint32_t joyride_tex_pal_slot(uint32_t pal);

/* At a hooked texel fetch (joyride_texfilter nonzero): `dst` is the pixel
 * about to be written, `addr` the texel (page | v << 8 | u), `fu`/`fv` the
 * coordinates' 16-bit fractions, `pal` the shading table (texel in bits
 * 8..15), `tr` whether colour 0 is skipped. On the CPU path, blend now; on
 * the GPU path, record the pixel (layout in texfilter.c). */
static inline __attribute__((always_inline))
void joyride_tex_record(uint32_t dst, uint32_t addr, uint32_t fu, uint32_t fv, uint32_t pal, uint32_t tr) {
    if (joyride_texfilter != 2) { joyride_texc = joyride_bilerp32(addr, fu, fv, pal, tr); return; }
    uint32_t t = JR_G8(addr);
    if (tr && !t) return;                                    /* the loop skips this texel */
    uint32_t off = dst - JR_G32(joyride_rows), i = off >> 2;      /* from row 0 of the surface drawn into */
    if ((off & 3) | (i >= joyride_gb_n)) return;
    pal &= 0xFFFF00FFu;
    uint32_t page = addr & ~0xFFFFu;
    uint32_t ps = page == joyride_gb_last_page ? joyride_gb_last_page_slot : joyride_tex_page_slot(page);
    uint32_t qs = pal == joyride_gb_last_pal ? joyride_gb_last_pal_slot : joyride_tex_pal_slot(pal);
    uint32_t c = JR_G32(pal + (t << 8)) & 0xFFFFFFu;
    c = (c ^ (c >> 17) ^ (c << 7)) & 0x1FFFFu;               /* the check (shaders/texfilter.frag) */
    uint32_t u = (addr & 0xFF) << 6 | (fu & 0xFFFF) >> 10, v = (addr >> 8 & 0xFF) << 6 | (fv & 0xFFFF) >> 10;
    uint32_t *e = joyride_gb + 2 * i;
    e[0] = u | v << 14 | (c >> 13) << 28;
    e[1] = ps | qs << 7 | tr << 17 | joyride_texclamp << 18 | (c & 0x1FFFu) << 19;
    joyride_gb_used = 1;
}

#endif
