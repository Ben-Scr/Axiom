$input  a_position, a_texcoord0, a_color0
$output v_texcoord0, v_color0

// ImGui vertex shader — Stage 3 of the bgfx port.
//
// ImGui submits draw lists with vertex format
//   position (vec2)  — pixel coords with origin top-left
//   uv       (vec2)
//   color    (rgba8 packed → comes through as vec4 after bgfx attribute decode)
//
// The orthographic transform that maps pixel coords into clip-space lives
// in u_imgui_ortho (a vec4 uniform: (left, right, top, bottom) / pixel
// extent). Built each frame in imgui_impl_bgfx::RenderDrawData.

#include <bgfx_shader.sh>

uniform vec4 u_imgui_ortho;

void main()
{
	float L = u_imgui_ortho.x;
	float R = u_imgui_ortho.y;
	float T = u_imgui_ortho.z;
	float B = u_imgui_ortho.w;

	float ndcX = ((a_position.x - L) / (R - L)) * 2.0 - 1.0;
	float ndcY = 1.0 - ((a_position.y - T) / (B - T)) * 2.0;

	gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);
	v_texcoord0 = a_texcoord0;
	v_color0    = a_color0;
}
