#version 450
// joyride - the city map over the game (src/platform/map.c): the map,
// scaled to fit the picture and centred, at 70%; the player a bright blue
// dot, objectives bright red, each with a dark rim so they read on any
// part of the map.

layout(set = 2, binding = 0) uniform sampler2D map;
layout(set = 3, binding = 0) uniform Params {
    vec4 rect;        // the map in target pixels: x, y, size, dot radius
    vec4 player;      // x, y (0..1 of the map), shown (0/1)
    vec4 obj[8];      // x, y
    vec4 count;       // number of objectives
};

layout(location = 0) out vec4 o;

float dot_at(vec2 p, vec2 c, float r) {          // 1 inside, a soft edge
    return 1.0 - smoothstep(r - 1.0, r + 1.0, distance(p, c));
}

void main() {
    vec2 p = gl_FragCoord.xy;
    vec2 uv = (p - rect.xy) / rect.z;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) discard;
    vec3 c = texture(map, uv).rgb;
    float a = 0.7;
    float r = rect.w;
    for (int i = 0; i < int(count.x) && i < 8; i++) {
        vec2 q = rect.xy + obj[i].xy * rect.z;
        float rim = dot_at(p, q, r + 2.0), fill = dot_at(p, q, r);
        c = mix(c, vec3(0.0), rim); c = mix(c, vec3(1.0, 0.1, 0.1), fill);
        a = mix(a, 1.0, rim);
    }
    if (player.z > 0.5) {
        vec2 q = rect.xy + player.xy * rect.z;
        float rim = dot_at(p, q, r + 2.0), fill = dot_at(p, q, r);
        c = mix(c, vec3(0.0), rim); c = mix(c, vec3(0.2, 0.6, 1.0), fill);
        a = mix(a, 1.0, rim);
    }
    o = vec4(c, a);
}
