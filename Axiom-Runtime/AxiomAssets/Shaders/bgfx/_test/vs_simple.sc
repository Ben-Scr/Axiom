$input  a_position, a_texcoord0, a_color0
$output v_uv, v_color

#include <bgfx_shader.sh>

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	v_uv    = a_texcoord0;
	v_color = a_color0;
}
