#version 450
// joyride - the hardware renderer's quads (src/hle/hwrender.c).
//
// The software fills a quad in spans along one axis: each scanline crosses
// two of its edges, the texture is interpolated along those edges, then
// across the span. This does the same for each pixel, so the mapping is
// the game's own (no seam from splitting the quad in two triangles).
// Then the texel (bilinear like the 3dfx card, or nearest), through the
// quad's shading table; colour 0 see-through where the quad says so (with
// filtering, as the 3dfx card's alpha test: kept where most of the weight
// is not see-through); blended quads come out at half alpha (the game's
// blend table is an exact average).

struct Quad {
    float x[4], y[4];
    float u[4], v[4];
    uint page, pal, mode, pad;
};
layout(set = 2, binding = 0) uniform usampler2DArray pages;
layout(set = 2, binding = 1) uniform sampler2D pals;
layout(std430, set = 2, binding = 2) readonly buffer Quads { Quad q[]; };
layout(set = 3, binding = 0) uniform Params { vec4 prm; };   // scale, filter (0/1)

layout(location = 0) flat in uint qi;
layout(location = 0) out vec4 o;

const uint VERTICAL = 1u, KEYED = 2u, BLEND = 4u;

uint texel(uint layer, ivec2 t) { return texelFetch(pages, ivec3(t, int(layer)), 0).r; }

void main() {
    uint m = q[qi].mode;
    bool vert = (m & VERTICAL) != 0u;
    // the game's pixel coordinates: whole numbers at pixel centres, as its
    // rasterizer counts them (a span's first pixel is its edge's x). Against
    // the software's own frame this leaves only single-pixel disagreements
    // on texel boundaries (its 16.16 steps against floats), none visible.
    vec2 p = (gl_FragCoord.xy - 0.5) / prm.x;
    float s = vert ? p.x : p.y, c = vert ? p.y : p.x; // along the scan axis, across the span

    float cmin = 1e9, cmax = -1e9;
    vec2 uvmin = vec2(0.0), uvmax = vec2(0.0);
    vec2 lo = vec2(1e9), hi = vec2(-1e9);
    for (int e = 0; e < 4; e++) {
        int f = (e + 1) & 3;
        vec2 ue = vec2(q[qi].u[e], q[qi].v[e]), uf = vec2(q[qi].u[f], q[qi].v[f]);
        lo = min(lo, ue), hi = max(hi, ue);
        float ae = vert ? q[qi].x[e] : q[qi].y[e], af = vert ? q[qi].x[f] : q[qi].y[f];
        float be = vert ? q[qi].y[e] : q[qi].x[e], bf = vert ? q[qi].y[f] : q[qi].x[f];
        if (ae == af) continue;
        float t = (s - ae) / (af - ae);
        if (t < 0.0 || t > 1.0) continue;
        float x = mix(be, bf, t);
        vec2 uv = mix(ue, uf, t);
        if (x < cmin) { cmin = x; uvmin = uv; }
        if (x > cmax) { cmax = x; uvmax = uv; }
    }
    if (cmax < cmin) discard;
    float k = cmax > cmin ? clamp((c - cmin) / (cmax - cmin), 0.0, 1.0) : 0.0;
    vec2 uv = mix(uvmin, uvmax, k);
    // the texels this quad uses, so filtering never reaches the next one
    ivec2 tlo = ivec2(floor(lo)), thi = ivec2(ceil(hi)) - 1;
    thi = max(thi, tlo);
    uint layer = q[qi].page;
    int pal = int(q[qi].pal);
    bool keyed = (m & KEYED) != 0u;
    vec3 rgb;
    if (prm.y > 0.5) {
        vec2 g = uv - 0.5;
        ivec2 i0 = ivec2(floor(g));
        vec2 w = g - vec2(i0);
        ivec2 a = clamp(i0, tlo, thi), b = clamp(i0 + 1, tlo, thi);
        uint t[4] = uint[4](texel(layer, a), texel(layer, ivec2(b.x, a.y)), texel(layer, ivec2(a.x, b.y)), texel(layer, b));
        float wt[4] = float[4]((1.0 - w.x) * (1.0 - w.y), w.x * (1.0 - w.y), (1.0 - w.x) * w.y, w.x * w.y);
        vec3 sum = vec3(0.0);
        float total = 0.0;
        for (int i = 0; i < 4; i++) {
            if (keyed && t[i] == 0u) continue;
            sum += texelFetch(pals, ivec2(int(t[i]), pal), 0).rgb * wt[i];
            total += wt[i];
        }
        if (total < (keyed ? 0.5 : 1e-6)) discard;
        rgb = sum / total;
    } else {
        uint t = texel(layer, clamp(ivec2(floor(uv)), tlo, thi));
        if (keyed && t == 0u) discard;
        rgb = texelFetch(pals, ivec2(int(t), pal), 0).rgb;
    }
    o = vec4(rgb, (m & BLEND) != 0u ? 0.5 : 1.0);
}
