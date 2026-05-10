$input v_texcoord0, v_color0

// Sprite fragment shader — bgfx port (Stage 2.1).

#include <bgfx_shader.sh>

SAMPLER2D(s_albedo, 0);

void main()
{
	gl_FragColor = texture2D(s_albedo, v_texcoord0) * v_color0;
}
