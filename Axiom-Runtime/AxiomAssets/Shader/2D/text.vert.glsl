#version 410 core

// Position is already in world-space pixels (text-renderer pre-applies
// the text component's scale + per-glyph layout offsets). The renderer
// pushes a single MVP per draw — no per-instance transform.
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uMVP;

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uMVP * vec4(aPosition, 0.0, 1.0);
}
