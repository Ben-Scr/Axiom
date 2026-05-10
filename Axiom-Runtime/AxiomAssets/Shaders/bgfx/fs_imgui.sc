$input v_texcoord0, v_color0

// ImGui fragment shader — Stage 3 of the bgfx port.
//
// Single sampler (the imgui font + image atlas), modulated by the
// per-vertex tint colour. Premultiplied alpha is the right choice for
// imgui's atlas; bgfx's blend state is BGFX_STATE_BLEND_FUNC_SEPARATE
// configured matching imgui's GL backend (src=alpha, dst=1-alpha for RGB;
// src=1-alpha, dst=1-alpha for A) inside imgui_impl_bgfx.

#include <bgfx_shader.sh>

SAMPLER2D(s_imgui, 0);

void main()
{
	gl_FragColor = texture2D(s_imgui, v_texcoord0) * v_color0;
}
