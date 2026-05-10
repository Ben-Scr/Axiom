#pragma once
#include "Core/UUID.hpp"
#include "Graphics/Filter.hpp"
#include "Graphics/TextureHandle.hpp"
#include "Collections/Color.hpp"

#include <cstdint>

namespace Axiom {
	struct ImageComponent {
		TextureHandle TextureHandle;
		UUID TextureAssetId{ 0 };
		Color Color;
		// Same shape as SpriteRendererComponent so the inspector / serializer /
		// script API can mirror that component verbatim. GuiRenderer sorts
		// images first by SortingLayer, then by SortingOrder, with hierarchy
		// walk order as the final tiebreaker (so siblings keep author order
		// when explicit sort fields tie).
		int16_t SortingOrder{ 0 };
		uint8_t SortingLayer{ 0 };
		// Sampler filter applied to the bound texture (Point/Bilinear/...).
		// Bilinear is the UI-friendly default — pixel-art workflows can
		// switch to Point per-image. The filter is applied to the underlying
		// Texture2D via TextureManager when the inspector setter / scripting
		// API mutates this value, so cross-references with the same texture
		// see a consistent sampler.
		Filter FilterMode{ Filter::Bilinear };
	};
}
