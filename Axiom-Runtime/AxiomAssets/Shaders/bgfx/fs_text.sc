$input v_texcoord0, v_color0

// Text fragment shader — bgfx port (Stage 2).
//
// Atlas is single-channel R8 (built by Font_Bgfx::BakeAtlas via stb_truetype).
// stb writes glyph coverage into the .r channel, so we sample it and use it
// as the alpha multiplier — RGB comes entirely from the per-vertex tint.
// This matches the OpenGL text shader's behaviour: glyphs have the colour
// the author asked for, with anti-aliased edges fading to fully transparent.

#include <bgfx_shader.sh>

SAMPLER2D(s_atlas, 0);

void main()
{
	float coverage = texture2D(s_atlas, v_texcoord0).r;
	gl_FragColor   = vec4(v_color0.rgb, v_color0.a * coverage);
}
