#version 450
// joyride - the hardware renderer's last step: what the CPU still drew
// (HUD, text) over the GPU's world. The surface was cleared to a key
// colour before the draw; the presenter turns that into alpha 0.

layout(set = 2, binding = 0) uniform sampler2D frame;
layout(set = 3, binding = 0) uniform Params { vec4 prm; };   // scale

layout(location = 0) out vec4 o;

void main() {
    vec4 c = texelFetch(frame, ivec2(gl_FragCoord.xy / prm.x), 0);
    if (c.a < 0.5) discard;
    o = vec4(c.rgb, 1.0);
}
