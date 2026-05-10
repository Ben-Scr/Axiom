$input  a_position, a_texcoord0, a_color0
$output v_texcoord0, v_color0

// Text vertex shader — bgfx port (Stage 2).
//
// Glyph quads are built CPU-side by TextRenderer_Bgfx::EmitText (six verts
// per visible glyph, already rotated/scaled). The shader's only job is to
// project them through the active view's u_viewProj matrix and forward
// uv + color to the fragment stage.
//
// `a_position` is xy only — z is implicitly 0 (text is 2D and the
// projection's z-range is whatever the caller set the bgfx view to).

#include <bgfx_shader.sh>

void main()
{
	gl_Position = mul(u_viewProj, vec4(a_position.xy, 0.0, 1.0));
	v_texcoord0 = a_texcoord0;
	v_color0    = a_color0;
}
