#pragma once

#include "Serialization/Path.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>

namespace Axiom {

	inline constexpr float k_AxiomImGuiFontSize = 15.0f;

	inline ImFont* LoadAxiomImGuiFont(ImGuiIO& io, float dpiScale = 1.0f) {
		const float fontSize = k_AxiomImGuiFontSize * dpiScale;

		ImFontConfig fontCfg;
		fontCfg.SizePixels = fontSize;
		fontCfg.PixelSnapH = true;

		const std::string notoPath = Path::Combine(Path::ResolveAxiomAssets("Fonts"), "GoogleSans", "GoogleSans-Regular.ttf");
		if (std::filesystem::exists(notoPath)) {
			if (ImFont* font = io.Fonts->AddFontFromFileTTF(
					notoPath.c_str(), fontSize, &fontCfg, io.Fonts->GetGlyphRangesDefault())) {
				return font;
			}
		}

		ImFontConfig fallbackCfg;
		fallbackCfg.SizePixels = fontSize;
		fallbackCfg.PixelSnapH = true;
		return io.Fonts->AddFontDefault(&fallbackCfg);
	}

}
