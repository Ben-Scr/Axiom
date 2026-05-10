$input  a_position, a_color0
$output v_color0

// Gizmo line vertex shader. Lines are submitted with BGFX_STATE_PT_LINES
// and BGFX_STATE_BLEND_ALPHA so the per-vertex alpha controls visibility
// (Gizmo callers tint translucent overlays at lower alpha). Position is
// already in world space — the gizmo renderer hands the editor camera's
// view-projection matrix to bgfx::setViewTransform, so a plain `mul`
// is all the vertex shader has to do.

#include <bgfx_shader.sh>

void main()
{
	gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
	v_color0    = a_color0;
}
