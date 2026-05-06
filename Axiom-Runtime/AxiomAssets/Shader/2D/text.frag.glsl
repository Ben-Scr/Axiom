#version 410 core

// Atlas is single-channel R8 alpha; we modulate the per-vertex color by
// the sampled coverage. Standard non-premultiplied alpha output — the
// pipeline blend state handles compositing.
in vec2 vUV;
in vec4 vColor;

uniform sampler2D uAtlas;

out vec4 oColor;

void main() {
    float coverage = texture(uAtlas, vUV).r;
    oColor = vec4(vColor.rgb, vColor.a * coverage);
    if (oColor.a < 0.001) {
        discard;
    }
}
