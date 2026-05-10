$input  a_position, i_data0, i_data1, i_data2
$output v_texcoord0, v_color0

// Sprite vertex shader — bgfx port (Stage 2.1 + rotation extension).
//
// Per-instance data layout (matches the CPU-side InstanceData / Instance44):
//   i_data0.xy = world position of the quad centre
//   i_data0.zw = (Scale.x, Scale.y)
//   i_data1    = per-instance Color tint (rgba)
//   i_data2.xy = (cos(rotation), sin(rotation))
//   i_data2.zw = unused (reserved — leave at 0)
//
// The unit quad's local coords are in [-0.5, 0.5]². We:
//   1. multiply by Scale to get the rect's half-extents,
//   2. rotate by (cos, sin) so a tilted RectTransform2D's image rotates
//      around its centre,
//   3. translate by Position to place it in world space.
// UV is derived from the quad's local coord (a_position.xy + 0.5) so we
// don't need a separate UV vertex attribute.

#include <bgfx_shader.sh>

void main()
{
	vec2 local   = a_position.xy * i_data0.zw;
	vec2 cs      = i_data2.xy;
	vec2 rotated = vec2(local.x * cs.x - local.y * cs.y,
	                    local.x * cs.y + local.y * cs.x);
	vec2 worldXY = rotated + i_data0.xy;

	gl_Position  = mul(u_viewProj, vec4(worldXY, 0.0, 1.0));
	v_texcoord0  = a_position.xy + vec2(0.5, 0.5);
	v_color0     = i_data1;
}
