#pragma once

#include "Core/Log.hpp"
#include "Serialization/Path.hpp"
#include "Serialization/SpecialFolder.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>

namespace Index {

	inline constexpr float k_IndexImGuiFontSize = 15.0f;

	inline ImFont* LoadIndexImGuiFont(ImGuiIO& io, float dpiScale = 1.0f,
									  float baseSize = k_IndexImGuiFontSize) {
		const float fontSize = baseSize * dpiScale;

		// Explicit glyph ranges are required: each binary statically links
		// its own ImGui (the launcher in particular — see comments in
		// Index-Launcher/src/Gui/ImGuiContextLayer.cpp). The WebGPU backend's
		// ImGuiBackendFlags_RendererHasTextures only ends up on the engine
		// DLL's ImGui context, so the launcher's ImGui still bakes a static
		// atlas — meaning a nullptr range here would result in nothing being
		// baked for the merged font. Passing ranges keeps the static path
		// correct and is a no-op on the dynamic-atlas path.

		ImFontConfig primaryCfg;
		primaryCfg.SizePixels = fontSize;
		primaryCfg.PixelSnapH = true;

		ImFont* primary = nullptr;
		const std::string googleSansPath = Path::Combine(
			Path::ResolveIndexAssets("Fonts"), "GoogleSans", "GoogleSans-Regular.ttf");
		if (std::filesystem::exists(googleSansPath)) {
			primary = io.Fonts->AddFontFromFileTTF(
				googleSansPath.c_str(), fontSize, &primaryCfg,
				io.Fonts->GetGlyphRangesDefault());
		}

		// Merged CJK fallback. The Localization service downloads this font
		// on-demand to the user-writable location; we prefer that copy so
		// the bundle can ship without it. Fall back to the bundled IndexAssets
		// path for the dev-layout install (still useful while iterating).
		std::string userCjkPath;
		try {
			userCjkPath = (std::filesystem::path(Path::GetSpecialFolderPath(SpecialFolder::LocalAppData))
				/ "Index" / "Fonts" / "NotoSansCJK" / "NotoSansCJK-Regular.ttc").string();
		}
		catch (...) {
			userCjkPath.clear();
		}
		const std::string bundledCjkPath = Path::Combine(
			Path::ResolveIndexAssets("Fonts"), "NotoSansCJK", "NotoSansCJK-Regular.ttc");

		std::string cjkPath;
		if (!userCjkPath.empty() && std::filesystem::exists(userCjkPath)) {
			cjkPath = userCjkPath;
		}
		else if (std::filesystem::exists(bundledCjkPath)) {
			cjkPath = bundledCjkPath;
		}

		ImFontConfig cjkCfg;
		cjkCfg.SizePixels = fontSize;
		cjkCfg.PixelSnapH = true;
		cjkCfg.MergeMode = true;
		if (!cjkPath.empty()) {
			io.Fonts->AddFontFromFileTTF(
				cjkPath.c_str(), fontSize, &cjkCfg,
				io.Fonts->GetGlyphRangesJapanese());
		}
		else {
			IDX_CORE_WARN_TAG("Localization",
				"CJK font not installed; CJK characters will render as boxes. "
				"It will be downloaded the first time you select a CJK language.");
		}

		if (primary) return primary;

		ImFontConfig fallbackCfg;
		fallbackCfg.SizePixels = fontSize;
		fallbackCfg.PixelSnapH = true;
		return io.Fonts->AddFontDefault(&fallbackCfg);
	}

}
