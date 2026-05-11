#pragma once
#include "Collections/Vec2.hpp"
#include "Core/Export.hpp"
#include "Graphics/Filter.hpp"
#include "Graphics/ImageData.hpp"
#include "Graphics/Wrap.hpp"

#include <cstdint>
#include <string>

namespace Axiom {
	class AXIOM_API Texture2D {
	public:
		Texture2D() = default;
		Texture2D(const char* path,
			Filter filter = Filter::Point,
			Wrap wrapU = Wrap::Clamp,
			Wrap wrapV = Wrap::Clamp,
			bool generateMipmaps = true,
			bool srgb = false,
			bool flipVertical = true);

		~Texture2D();

		Texture2D(const Texture2D&) = delete;
		Texture2D& operator=(const Texture2D&) = delete;
		Texture2D(Texture2D&&) noexcept;
		Texture2D& operator=(Texture2D&&) noexcept;

		void Destroy();

		bool Load(const char* path,
			bool generateMipmaps = true,
			bool srgb = false,
			bool flipVertical = true);

		void Submit(uint8_t unit) const;

		void SetFilter(Filter filter);
		void SetWrapU(Wrap u);
		void SetWrapV(Wrap v);
		void SetSampler(Filter filter, Wrap u = Wrap::Clamp, Wrap v = Wrap::Clamp);

		Filter GetFilter() const { return m_Filter; }
		Wrap GetWrapU() const { return m_WrapU; }
		Wrap GetWrapV() const { return m_WrapV; }

		bool IsValid() const { return m_Tex != 0; }

		std::unique_ptr<ImageData> GetImageData() const;
		// Returns the WGPU texture-view raw pointer cast to uint64_t under
		// the WebGPU backend. ImGui's imgui_impl_wgpu reinterpret-casts
		// ImTextureID directly to WGPUTextureView and dereferences it, so
		// the editor's ImGui::Image / ImageButton call sites that pass
		// `tex->GetHandle()` need a real Dawn handle here -- not a small
		// integer pool counter (which Dawn would dereference at address
		// 0x1, 0x2, etc. and crash with a 0xC0000005 access violation).
		uint64_t GetHandle() const { return m_Tex; }
		Vec2 Size() const { return Vec2{ float(m_Width), float(m_Height) }; }
		float GetWidth() const { return m_Width; }
		float GetHeight() const { return m_Height; }

		float AspectRatio() const { return m_Height != 0 ? float(m_Width) / float(m_Height) : 0.0f; }

	private:
		// Under WebGPU this holds the raw WGPUTextureView pointer (cast to
		// uint64_t). 0 stays the "unset" sentinel; IsValid() compares
		// against 0 unchanged. The Texture2D_WebGPU.cpp pool uses this
		// same value as its key.
		uint64_t m_Tex = 0;
		int m_Width = 0, m_Height = 0, m_Channels = 0;

		Filter m_Filter = Filter::Point;
		Wrap   m_WrapU = Wrap::Clamp;
		Wrap   m_WrapV = Wrap::Clamp;
		bool   m_HasMips = true;
		void ApplySamplerParams() const;
	};

} // namespace Axiom
