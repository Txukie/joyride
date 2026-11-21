#version 450
// joyride - bilinear texture filtering on the GPU, the 3dfx build's look
// (docs/exe-3dfx.md, src/hle/texfilter.c).
//
// The game rasterizes on the CPU as always; its textured span loops also
// record, per pixel, where the texel came from (texfilter.c). Here each
// recorded pixel is re-sampled from the four texels around that point,
// each through the face's shading table, exactly as the CPU filter does.
//
//   frame  the game's picture (BGRA8)
//   gbuf   per pixel: x = u | v << 14 | check >> 13 << 28, u and v 8.6
//          in the 256x256 page; y = page slot (7 bits, 0 = not recorded) |
//          palette slot << 7 (10) | transparent << 17 | tile << 18 |
//          check << 19 (its low 13 bits); the check is 17 bits of the
//          colour the game wrote there
//   pages  the 8-bit texture pages, one layer per slot
//   pals   the shading tables: row = slot, column = texel
// A pixel whose colour no longer matches its check was drawn over later
// (HUD, text, translucent effects) and is left as the game drew it.

layout(set = 2, binding = 0) uniform sampler2D frame;
layout(set = 2, binding = 1) uniform usampler2D gbuf;
layout(set = 2, binding = 2) uniform usampler2DArray pages;
layout(set = 2, binding = 3) uniform sampler2D pals;

layout(location = 0) out vec4 o;

// one coordinate: the two texel columns/rows around it, clamped to the
// 64-texel tile (world textures) or the page (sprites), and the weight of
// the second
void split(uint q, bool tile, out int c0, out int c1, out float w) {
    int i = int(q >> 6);
    int lo = tile ? (i & ~63) : 0, hi = tile ? lo + 63 : 255;
    int p = int(q) - 32;                           // texel centres
    int k = p >> 6;
    w = float(p & 63) / 64.0;
    if (k < lo) { k = lo; w = 0.0; }
    if (k >= hi) { k = hi; w = 0.0; }
    c0 = k;
    c1 = min(k + 1, 255);
}

void main() {
    ivec2 at = ivec2(gl_FragCoord.xy);
    vec4 c = texelFetch(frame, at, 0);
    uvec2 g = texelFetch(gbuf, at, 0).xy;
    uint layer = g.y & 127u;
    if (layer == 0u) { o = c; return; }

    uvec3 b = uvec3(round(c.rgb * 255.0));
    uint col = b.r << 16 | b.g << 8 | b.b;
    uint check = (col ^ (col >> 17) ^ (col << 7)) & 0x1FFFFu;
    if (check != ((g.x >> 28) << 13 | g.y >> 19)) { o = c; return; }

    int pal = int((g.y >> 7) & 1023u);
    bool transparent = ((g.y >> 17) & 1u) != 0u, tile = ((g.y >> 18) & 1u) != 0u;
    int u0, u1, v0, v1;
    float a, bw;
    split(g.x & 0x3FFFu, tile, u0, u1, a);
    split((g.x >> 14) & 0x3FFFu, tile, v0, v1, bw);

    uint t[4] = uint[4](texelFetch(pages, ivec3(u0, v0, layer), 0).r, texelFetch(pages, ivec3(u1, v0, layer), 0).r,
                        texelFetch(pages, ivec3(u0, v1, layer), 0).r, texelFetch(pages, ivec3(u1, v1, layer), 0).r);
    float w[4] = float[4]((1.0 - a) * (1.0 - bw), a * (1.0 - bw), (1.0 - a) * bw, a * bw);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int i = 0; i < 4; i++) {
        if (transparent && t[i] == 0u) continue;   // colour 0 is see-through: no weight
        sum += texelFetch(pals, ivec2(int(t[i]), pal), 0).rgb * w[i];
        total += w[i];
    }
    o = total > 0.0 ? vec4(sum / total, 1.0) : c;
}
