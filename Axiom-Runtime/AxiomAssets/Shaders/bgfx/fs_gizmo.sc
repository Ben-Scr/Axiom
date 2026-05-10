$input v_color0

// Gizmo fragment shader. Pure pass-through tint — the vertex stage already
// did the projection, lines have no texture, so the per-vertex RGBA goes
// straight out.

#include <bgfx_shader.sh>

void main()
{
	gl_FragColor = v_color0;
}
