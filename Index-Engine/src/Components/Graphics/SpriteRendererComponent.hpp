#pragma once
#include "Core/Export.hpp"
#include "Core/UUID.hpp"
#include "Graphics/Filter.hpp"
#include "Graphics/TextureHandle.hpp"
#include "Collections/Color.hpp"

namespace Index {
	struct INDEX_API SpriteRendererComponent {
		SpriteRendererComponent() = default;

		short SortingOrder{0};
		uint8_t SortingLayer{0};
		TextureHandle TextureHandle;
		UUID TextureAssetId{ 0 };
		Color Color{ 1.0f, 1.0f, 1.0f, 1.0f };
		// Sampler filter applied to the bound texture. Inspector setter
		// and SpriteRenderer_SetFilter script binding both call
		// Texture2D::SetFilter so the underlying sampler is regenerated;
		// scene load also reapplies via LoadTextureFromValue.
		Filter FilterMode{ Filter::Bilinear };
	};
}
