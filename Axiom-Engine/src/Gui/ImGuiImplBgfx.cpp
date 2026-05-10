#include "pch.hpp"
#include "Gui/ImGuiImplBgfx.hpp"

#include "Core/Log.hpp"
#include "Graphics/Shader.hpp"
#include "Packages/PackageImGuiBridge.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Path.hpp"

#include <bgfx/bgfx.h>
#include <imgui.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

// =============================================================================
// imgui_impl_bgfx — Stage 3 of the bgfx port.
// -----------------------------------------------------------------------------
// Minimal ImGui backend that submits draw lists through bgfx. Built only
// under --rhi=bgfx; Stage 0/1's premake gate selects between this backend
// and imgui_impl_opengl3 via the AIM_RHI_BGFX define.
//
// Resources owned by this TU:
//   * `g_Program` — bgfx::ProgramHandle for vs_imgui + fs_imgui.
//   * `g_FontTex` — bgfx::TextureHandle for ImGui's font atlas.
//   * `g_SamplerUniform` — sampler uniform binding the texture per draw.
//   * `g_OrthoUniform` — vec4 (L, R, T, B) screen rect per frame.
//   * `g_VertexLayout` — pos2, uv2, rgba8.
//
// Per-frame buffer strategy: bgfx::TransientVertexBuffer +
// bgfx::TransientIndexBuffer. One pair allocated per ImDrawList; the
// transient allocator releases them at frame end (no manual cleanup).
// =============================================================================

namespace Axiom::ImGuiImplBgfx {

	namespace {
		bool                       g_Initialized   = false;
		bgfx::ProgramHandle        g_Program       = BGFX_INVALID_HANDLE;
		bgfx::TextureHandle        g_FontTex       = BGFX_INVALID_HANDLE;
		bgfx::UniformHandle        g_SamplerUniform = BGFX_INVALID_HANDLE;
		bgfx::UniformHandle        g_OrthoUniform   = BGFX_INVALID_HANDLE;
		bgfx::VertexLayout         g_VertexLayout;

		// Engine.dll has its own statically-linked copy of ImGui (added under
		// --rhi=bgfx so this backend can call into ImGui APIs). That copy has
		// its own GImGui pointer / allocator funcs which start out null. The
		// consumer (editor / launcher / runtime) creates the active ImGuiContext
		// in its own copy of ImGui and publishes the pointer through
		// PackageImGuiBridge. We sync engine.dll's ImGui state to point at the
		// same context+allocators on every entry point. The bridge's generation
		// counter lets us skip the SetCurrent calls when nothing changed.
		unsigned long long g_LastSyncedGen = 0;

		void SyncImGuiContextFromBridge() {
			const unsigned long long gen = PackageImGuiBridge::GetGeneration();
			if (gen == g_LastSyncedGen && ImGui::GetCurrentContext() != nullptr) {
				return;
			}
			void* ctx = PackageImGuiBridge::GetContext();
			if (ctx == nullptr) {
				return; // Consumer hasn't published yet — Init will be a no-op.
			}
			ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));
			void* allocFn = nullptr;
			void* freeFn  = nullptr;
			void* user    = nullptr;
			PackageImGuiBridge::GetAllocators(allocFn, freeFn, user);
			if (allocFn != nullptr && freeFn != nullptr) {
				ImGui::SetAllocatorFunctions(
					reinterpret_cast<ImGuiMemAllocFunc>(allocFn),
					reinterpret_cast<ImGuiMemFreeFunc>(freeFn),
					user);
			}
			g_LastSyncedGen = gen;
		}

		std::string ResolveImguiBin(const char* stagePrefix) {
			// Mirror Shader_Bgfx.cpp's resolve order, in this priority:
			//   1. `<exe>/AxiomAssets/Shaders/bgfx/bin/<profile>/...`
			//      — the postbuild-copied target dir.
			//   2. `Path::ResolveAxiomAssets("Shaders/bgfx/bin/<profile>")
			//      / <stage>_imgui.bin` — exe-adjacent + dev `bin/<config>/`
			//      fallback.
			//   3. Bare relative path (kept for parity with the historical
			//      behaviour; mostly only useful when the cwd happens to
			//      be the repo root).
			const char* profile = nullptr;
			switch (bgfx::getRendererType()) {
			case bgfx::RendererType::Direct3D11:
			case bgfx::RendererType::Direct3D12: profile = "dx11"; break;
			case bgfx::RendererType::OpenGL:
			case bgfx::RendererType::OpenGLES:   profile = "glsl"; break;
			case bgfx::RendererType::Vulkan:     profile = "spirv"; break;
			default: return std::string();
			}

			const std::string filename = std::string(stagePrefix) + "_imgui.bin";
			const std::string subdir = std::string("Shaders/bgfx/bin/") + profile;

			// (1) Exe-adjacent (the most common shipped layout).
			std::string exeRel = Path::Combine(Path::ExecutableDir(),
				"AxiomAssets/" + subdir, filename);
			if (std::filesystem::exists(exeRel)) return exeRel;

			// (2) ResolveAxiomAssets covers both exe-adjacent and the
			// dev `bin/<config>/AxiomAssets/` layout one level up. Re-
			// running (1) via this path is harmless (same file,
			// different lookup) and the second branch finds it when the
			// editor is launched from `bin/Debug-windows-x86_64/Axiom-
			// Editor/` and the postbuild hasn't yet re-run for this
			// config.
			std::string assetDir = Path::ResolveAxiomAssets(subdir);
			if (!assetDir.empty()) {
				std::string p = Path::Combine(assetDir, filename);
				if (std::filesystem::exists(p)) return p;
			}

			// (3) Final relative fallback.
			return std::string("AxiomAssets/") + subdir + "/" + filename;
		}

		bgfx::ShaderHandle LoadStage(const char* stagePrefix) {
			const std::string path = ResolveImguiBin(stagePrefix);
			// Diagnostic line BEFORE the existence check so a "missing"
			// failure leaves an actionable trail. Logs whether
			// ResolveImguiBin produced any candidate at all (empty
			// string means bgfx::getRendererType() returned a renderer
			// the switch doesn't map — Noop, Metal, etc.) and prints
			// the bgfx renderer type integer so a mismatch between the
			// "bgfx initialized — renderer=X" line and the type seen
			// here is visible.
			AIM_CORE_INFO_TAG("ImGuiImplBgfx",
				"LoadStage('{}'): renderer={}, candidate path={}",
				stagePrefix,
				static_cast<int>(bgfx::getRendererType()),
				path.empty() ? std::string("<empty>") : path);
			if (path.empty() || !std::filesystem::exists(path)) {
				AIM_CORE_WARN_TAG("ImGuiImplBgfx", "imgui shader missing: {}", path);
				return BGFX_INVALID_HANDLE;
			}
			std::vector<uint8_t> bytes = File::ReadAllBytes(path);
			if (bytes.empty()) {
				AIM_CORE_WARN_TAG("ImGuiImplBgfx", "imgui shader empty: {}", path);
				return BGFX_INVALID_HANDLE;
			}
			const bgfx::Memory* mem = bgfx::copy(bytes.data(),
				static_cast<uint32_t>(bytes.size()));
			bgfx::ShaderHandle h = bgfx::createShader(mem);
			if (!bgfx::isValid(h)) {
				AIM_CORE_ERROR_TAG("ImGuiImplBgfx", "bgfx::createShader failed: {}", path);
			}
			return h;
		}

		// Encode (bgfx::TextureHandle::idx + 1) into ImTextureID so the
		// "no texture" sentinel stays at 0 even when bgfx hands out
		// handle idx 0. ImTextureID is `ImU64` in current imgui (was
		// `void*` pre-1.91), so plain integer casts work — no
		// reinterpret_cast needed.
		constexpr ImTextureID EncodeTex(uint16_t idx) noexcept {
			return static_cast<ImTextureID>(idx) + 1u;
		}
		constexpr uint16_t DecodeTex(ImTextureID id) noexcept {
			return static_cast<uint16_t>(id - 1u);
		}

		void EnsureFontTexture() {
			ImGuiIO& io = ImGui::GetIO();
			unsigned char* pixels = nullptr;
			int width = 0, height = 0;
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
			if (!pixels || width <= 0 || height <= 0) return;

			// Reuse the existing texture if the atlas hasn't been
			// rebuilt since the last upload. imgui's TexRef sits on the
			// atlas's primary texture data; SetTexID propagates our
			// bgfx-encoded handle so subsequent draws find it.
			if (bgfx::isValid(g_FontTex)) {
				const ImTextureID cached = io.Fonts->TexRef.GetTexID();
				if (cached == EncodeTex(g_FontTex.idx)) {
					return;
				}
				bgfx::destroy(g_FontTex);
				g_FontTex = BGFX_INVALID_HANDLE;
			}

			const uint32_t bytes = static_cast<uint32_t>(width)
				* static_cast<uint32_t>(height) * 4u;
			const bgfx::Memory* mem = bgfx::copy(pixels, bytes);
			g_FontTex = bgfx::createTexture2D(
				static_cast<uint16_t>(width),
				static_cast<uint16_t>(height),
				/*hasMips=*/false, /*numLayers=*/1,
				bgfx::TextureFormat::RGBA8,
				BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
				| BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
				mem);
			io.Fonts->SetTexID(EncodeTex(g_FontTex.idx));
		}
	}

	bool Init() {
		if (g_Initialized) return true;

		// Pull the consumer's ImGuiContext into engine.dll's ImGui copy
		// before any io.Fonts access in EnsureFontTexture below.
		SyncImGuiContextFromBridge();

		bgfx::ShaderHandle vs = LoadStage("vs");
		if (!bgfx::isValid(vs)) return false;
		bgfx::ShaderHandle fs = LoadStage("fs");
		if (!bgfx::isValid(fs)) {
			bgfx::destroy(vs);
			return false;
		}
		g_Program = bgfx::createProgram(vs, fs, /*destroyShaders=*/true);
		if (!bgfx::isValid(g_Program)) {
			AIM_CORE_ERROR_TAG("ImGuiImplBgfx", "bgfx::createProgram failed for imgui");
			return false;
		}

		g_SamplerUniform = bgfx::createUniform("s_imgui",       bgfx::UniformType::Sampler);
		g_OrthoUniform   = bgfx::createUniform("u_imgui_ortho", bgfx::UniformType::Vec4);

		// ImDrawVert: vec2 pos, vec2 uv, uint32 col (rgba8).
		g_VertexLayout
			.begin()
			.add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, /*normalized=*/true)
			.end();

		EnsureFontTexture();

		g_Initialized = true;
		return true;
	}

	void NewFrame() {
		if (!g_Initialized) return;
		// Resync in case the consumer hot-reloaded its ImGuiContext.
		SyncImGuiContextFromBridge();
		EnsureFontTexture(); // pick up font-atlas resizes between frames.
	}

	void RenderDrawData(ImDrawData* drawData, unsigned short viewId) {
		if (!g_Initialized || drawData == nullptr || drawData->CmdListsCount <= 0) {
			return;
		}
		const float fbW = drawData->DisplaySize.x * drawData->FramebufferScale.x;
		const float fbH = drawData->DisplaySize.y * drawData->FramebufferScale.y;
		if (fbW <= 0.0f || fbH <= 0.0f) return;

		// Diagnostic — fires once on first render and on every size
		// change. Surfaces the (DisplaySize, FramebufferScale, fb)
		// triple the bgfx view is being configured with so a divergence
		// between "what GLFW thinks the window is" and "what the user
		// sees" is visible in the log instead of having to guess from
		// screenshots. Costs nothing past the first stable frame.
		{
			static float s_LastFbW = -1.0f;
			static float s_LastFbH = -1.0f;
			if (fbW != s_LastFbW || fbH != s_LastFbH) {
				AIM_CORE_INFO_TAG("ImGuiImplBgfx",
					"RenderDrawData: DisplaySize={:.0f}x{:.0f}, FBScale={:.2f}x{:.2f}, view-rect={:.0f}x{:.0f}",
					drawData->DisplaySize.x, drawData->DisplaySize.y,
					drawData->FramebufferScale.x, drawData->FramebufferScale.y,
					fbW, fbH);
				s_LastFbW = fbW;
				s_LastFbH = fbH;
			}
		}

		const bgfx::ViewId v = static_cast<bgfx::ViewId>(viewId);
		bgfx::setViewName(v, "ImGui");
		bgfx::setViewMode(v, bgfx::ViewMode::Sequential);
		bgfx::setViewRect(v, 0, 0,
			static_cast<uint16_t>(fbW),
			static_cast<uint16_t>(fbH));
		// We don't clear here — the ImGui pass overlays whatever the
		// editor's per-panel views drew first.
		bgfx::setViewClear(v, BGFX_CLEAR_NONE);

		const float L = drawData->DisplayPos.x;
		const float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
		const float T = drawData->DisplayPos.y;
		const float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
		const float ortho[4] = { L, R, T, B };
		bgfx::setUniform(g_OrthoUniform, ortho);

		const ImVec2 clipOff   = drawData->DisplayPos;
		const ImVec2 clipScale = drawData->FramebufferScale;

		for (int n = 0; n < drawData->CmdListsCount; ++n) {
			const ImDrawList* drawList = drawData->CmdLists[n];

			const uint32_t numVertices = static_cast<uint32_t>(drawList->VtxBuffer.Size);
			const uint32_t numIndices  = static_cast<uint32_t>(drawList->IdxBuffer.Size);
			if (numVertices == 0 || numIndices == 0) continue;

			bgfx::TransientVertexBuffer tvb{};
			bgfx::TransientIndexBuffer  tib{};
			if (bgfx::getAvailTransientVertexBuffer(numVertices, g_VertexLayout) < numVertices
				|| bgfx::getAvailTransientIndexBuffer(numIndices, /*32-bit=*/sizeof(ImDrawIdx) == 4) < numIndices)
			{
				AIM_CORE_WARN_TAG("ImGuiImplBgfx",
					"transient buffer exhausted — dropping draw list (verts={}, indices={})",
					numVertices, numIndices);
				continue;
			}
			bgfx::allocTransientVertexBuffer(&tvb, numVertices, g_VertexLayout);
			bgfx::allocTransientIndexBuffer(&tib, numIndices, /*32-bit=*/sizeof(ImDrawIdx) == 4);
			std::memcpy(tvb.data, drawList->VtxBuffer.Data, numVertices * sizeof(ImDrawVert));
			std::memcpy(tib.data, drawList->IdxBuffer.Data, numIndices * sizeof(ImDrawIdx));

			uint32_t indexOffset = 0;
			for (int cmdIdx = 0; cmdIdx < drawList->CmdBuffer.Size; ++cmdIdx) {
				const ImDrawCmd& cmd = drawList->CmdBuffer[cmdIdx];
				if (cmd.UserCallback) {
					cmd.UserCallback(drawList, &cmd);
					indexOffset += cmd.ElemCount;
					continue;
				}
				if (cmd.ElemCount == 0) continue;

				// Clip rect → bgfx scissor.
				const float clipMinX = (cmd.ClipRect.x - clipOff.x) * clipScale.x;
				const float clipMinY = (cmd.ClipRect.y - clipOff.y) * clipScale.y;
				const float clipMaxX = (cmd.ClipRect.z - clipOff.x) * clipScale.x;
				const float clipMaxY = (cmd.ClipRect.w - clipOff.y) * clipScale.y;
				if (clipMaxX <= clipMinX || clipMaxY <= clipMinY) {
					indexOffset += cmd.ElemCount;
					continue;
				}
				const uint16_t scX = static_cast<uint16_t>(clipMinX < 0.f ? 0.f : clipMinX);
				const uint16_t scY = static_cast<uint16_t>(clipMinY < 0.f ? 0.f : clipMinY);
				const uint16_t scW = static_cast<uint16_t>(clipMaxX - clipMinX);
				const uint16_t scH = static_cast<uint16_t>(clipMaxY - clipMinY);

				bgfx::setScissor(scX, scY, scW, scH);

				bgfx::TextureHandle tex = g_FontTex;
				const ImTextureID texId = cmd.GetTexID();
				if (texId != 0) {
					// ImTextureID stores (idx + 1) per EnsureFontTexture's
					// convention; same encoding Texture2D / Framebuffer use.
					tex.idx = DecodeTex(texId);
				}
				bgfx::setTexture(0, g_SamplerUniform, tex);

				bgfx::setVertexBuffer(0, &tvb, 0, numVertices);
				bgfx::setIndexBuffer(&tib, indexOffset, cmd.ElemCount);

				bgfx::setState(0
					| BGFX_STATE_WRITE_RGB
					| BGFX_STATE_WRITE_A
					| BGFX_STATE_BLEND_FUNC_SEPARATE(
						BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA,
						BGFX_STATE_BLEND_ONE,       BGFX_STATE_BLEND_INV_SRC_ALPHA)
					| BGFX_STATE_MSAA);

				bgfx::submit(v, g_Program);

				indexOffset += cmd.ElemCount;
			}
		}
	}

	void Shutdown() {
		if (!g_Initialized) return;
		if (bgfx::isValid(g_Program))        bgfx::destroy(g_Program);
		if (bgfx::isValid(g_FontTex))        bgfx::destroy(g_FontTex);
		if (bgfx::isValid(g_SamplerUniform)) bgfx::destroy(g_SamplerUniform);
		if (bgfx::isValid(g_OrthoUniform))   bgfx::destroy(g_OrthoUniform);
		g_Program        = BGFX_INVALID_HANDLE;
		g_FontTex        = BGFX_INVALID_HANDLE;
		g_SamplerUniform = BGFX_INVALID_HANDLE;
		g_OrthoUniform   = BGFX_INVALID_HANDLE;
		g_Initialized = false;
	}

} // namespace Axiom::ImGuiImplBgfx
