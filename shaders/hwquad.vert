#version 450
// joyride - the hardware renderer's quads (src/hle/hwrender.c): one
// instance a quad, two triangles over its corners; the fragment shader
// works out the texture from the quad itself.

struct Quad {
    float x[4], y[4];       // corners, game pixels
    float u[4], v[4];       // page coordinates
    uint page, pal, mode, pad;
};
layout(std430, set = 0, binding = 0) readonly buffer Quads { Quad q[]; };
layout(set = 1, binding = 0) uniform View { vec4 view; };   // game width, height

layout(location = 0) flat out uint qi;

void main() {
    const int corner[6] = int[6](0, 1, 2, 0, 2, 3);
    uint i = uint(gl_InstanceIndex);
    int k = corner[gl_VertexIndex];
    vec2 p = vec2(q[i].x[k], q[i].y[k]) / view.xy;
    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
    qi = i;
}
