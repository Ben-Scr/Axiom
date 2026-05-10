$input v_texcoord0, v_color0

// ImGui fragment shader — Stage 3 of the bgfx port.
//
// Single sampler (the imgui font + image atlas), modulated by the
// per-vertex tint colour. bgfx's blend state is BGFX_STATE_BLEND_FUNC_SEPARATE
// configured matching imgui's GL backend (src=alpha, dst=1-alpha for RGB;
// src=ONE, dst=1-alpha for A) inside imgui_impl_bgfx — the ONE on srcA
// keeps the framebuffer alpha intact across stacked opaque draws so DWM
// doesn't ever see a transparent backbuffer.

#include <bgfx_shader.sh>

SAMPLER2D(s_imgui, 0);

void main()
{
	gl_FragColor = texture2D(s_imgui, v_texcoord0) * v_color0;
}
