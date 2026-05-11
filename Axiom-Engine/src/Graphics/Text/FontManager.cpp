#include "pch.hpp"
#include "Graphics/Text/FontManager.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Core/Log.hpp"
#include "Serialization/File.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

// =============================================================================
// FontManager — slot table + lookup-by-(uuid, quantized px).
// -----------------------------------------------------------------------------
// Constructs Font objects that bake their atlas through the active render
// backend. Quantization ladder is identical to the legacy OpenGL impl so a
// font dragged through size-N sliders allocates the same handful of
// atlases.
//
// What's intentionally lighter:
//   * No TTF-byte cache — re-reads each .ttf on every Load. The legacy
//     `s_TtfBufferCache` saves disk I/O across pixel-size bakes; if we
//     measure it as a real hot path we can lift it into a shared helper.
//     Today UI rebuilds are infrequent so the simpler path stays.
//   * No PurgeUnreferenced equivalent — the UnloadAll path frees
//     everything on shutdown / project switch, which matches the runtime
//     case.
// =============================================================================

namespace Axiom {

	namespace {
		int QuantizePixelSize(float pixelSize) {
			int p = std::max(1, static_cast<int>(std::lround(pixelSize)));
			auto snap = [](int v, int step) {
				return ((v + step / 2) / step) * step;
			};
			if (p <= 16)  return p;
			if (p <= 32)  return snap(p, 2);
			if (p <= 64)  return snap(p, 4);
			if (p <= 128) return snap(p, 8);
			return snap(p, 16);
		}
	}

	bool FontManager::Initialize() {
		if (s_IsInitialized) return true;

		s_Slots.clear();
		s_Lookup.clear();
		s_DefaultFont = FontHandle::Invalid();
		s_IsInitialized = true;

		// Same default-font registration the OpenGL impl does — keeps the
		// inspector's font picker populated for projects that ship none.
		std::string fontDir = Path::ResolveAxiomAssets("Fonts");
		if (fontDir.empty()) {
			fontDir = Path::Combine(Path::ExecutableDir(), "AxiomAssets", "Fonts");
		}
		const std::string defaultFontPath = Path::Combine(fontDir, "DefaultSans-Regular.ttf");
		if (std::filesystem::exists(defaultFontPath)) {
			std::error_code ec;
			const std::string canonical = std::filesystem::weakly_canonical(
				std::filesystem::path(defaultFontPath), ec).make_preferred().string();
			AssetRegistry::RegisterBuiltInAsset(
				ec ? defaultFontPath : canonical, k_DefaultFontAssetId, AssetKind::Font);
			AIM_CORE_INFO_TAG("FontManager", "Default font registered: {}",
				ec ? defaultFontPath : canonical);
		}
		else {
			AIM_CORE_WARN_TAG("FontManager", "Default font missing: {}", defaultFontPath);
		}
		return true;
	}

	void FontManager::Shutdown() {
		if (!s_IsInitialized) return;
		UnloadAll();
		s_TtfBufferCache.clear();
		s_DefaultFont = FontHandle::Invalid();
		s_IsInitialized = false;
	}

	FontHandle FontManager::LoadFont(std::string_view path, float pixelSize) {
		if (!s_IsInitialized || path.empty() || pixelSize <= 0.0f) {
			return FontHandle::Invalid();
		}
		const std::string pathStr(path);
		const uint64_t uuid = AssetRegistry::GetOrCreateAssetUUID(pathStr);
		if (uuid != 0) {
			return LoadFontByUUID(uuid, pixelSize);
		}
		// Fall through: load directly from path with a synthetic UUID
		// so the slot still has a stable key.
		auto font = std::make_unique<Font>();
		if (!font->LoadFromFile(pathStr, pixelSize)) {
			return FontHandle::Invalid();
		}
		return CreateSlot(std::move(font), 0, pixelSize);
	}

	FontHandle FontManager::LoadFontByUUID(uint64_t assetId, float pixelSize) {
		if (!s_IsInitialized || assetId == 0 || pixelSize <= 0.0f) {
			return FontHandle::Invalid();
		}

		FontHandle existing = FindExisting(assetId, pixelSize);
		if (existing.IsValid()) return existing;

		std::string path = AssetRegistry::ResolvePath(assetId);
		if (path.empty()) {
			AssetRegistry::MarkDirty();
			AssetRegistry::Sync();
			path = AssetRegistry::ResolvePath(assetId);
		}
		if (path.empty()) return FontHandle::Invalid();

		auto font = std::make_unique<Font>();
		if (!font->LoadFromFile(path, pixelSize)) {
			return FontHandle::Invalid();
		}
		return CreateSlot(std::move(font), assetId, pixelSize);
	}

	void FontManager::UnloadFont(const FontHandle& handle) {
		if (!IsValid(handle)) return;
		Slot& slot = s_Slots[handle.index];
		const LookupKey key{ slot.AssetUUID, QuantizePixelSize(slot.PixelSize) };
		auto it = s_Lookup.find(key);
		if (it != s_Lookup.end() && it->second == handle.index) {
			s_Lookup.erase(it);
		}
		slot.Font.reset();
		slot.AssetUUID = 0;
		slot.PixelSize = 0.0f;
		slot.InUse = false;
		++slot.Generation;
	}

	void FontManager::UnloadAll() {
		for (Slot& slot : s_Slots) {
			if (slot.InUse) {
				slot.Font.reset();
				slot.AssetUUID = 0;
				slot.PixelSize = 0.0f;
				slot.InUse = false;
				++slot.Generation;
			}
		}
		s_Lookup.clear();
		s_DefaultFont = FontHandle::Invalid();
	}

	bool FontManager::IsValid(const FontHandle& handle) {
		if (!s_IsInitialized) return false;
		if (!handle.IsValid()) return false;
		if (handle.index >= s_Slots.size()) return false;
		const Slot& slot = s_Slots[handle.index];
		return slot.InUse
			&& slot.Generation == handle.generation
			&& slot.Font
			&& slot.Font->IsLoaded();
	}

	Font* FontManager::GetFont(const FontHandle& handle) {
		if (!IsValid(handle)) return nullptr;
		return s_Slots[handle.index].Font.get();
	}

	uint64_t FontManager::GetFontAssetUUID(const FontHandle& handle) {
		if (!IsValid(handle)) return 0;
		return s_Slots[handle.index].AssetUUID;
	}

	FontHandle FontManager::GetDefaultFont() {
		if (!s_IsInitialized) return FontHandle::Invalid();
		if (IsValid(s_DefaultFont)) return s_DefaultFont;
		s_DefaultFont = LoadFontByUUID(k_DefaultFontAssetId, 32.0f);
		return s_DefaultFont;
	}

	FontHandle FontManager::CreateSlot(std::unique_ptr<Font> font, uint64_t assetUUID, float pixelSize) {
		if (!font) return FontHandle::Invalid();
		// Reuse a free slot if one exists, else append.
		uint16_t idx = static_cast<uint16_t>(s_Slots.size());
		for (size_t i = 0; i < s_Slots.size(); ++i) {
			if (!s_Slots[i].InUse) { idx = static_cast<uint16_t>(i); break; }
		}
		if (idx == s_Slots.size()) {
			s_Slots.emplace_back();
		}
		Slot& slot = s_Slots[idx];
		slot.Font = std::move(font);
		slot.AssetUUID = assetUUID;
		slot.PixelSize = pixelSize;
		slot.InUse = true;
		FontHandle h{ idx, slot.Generation };

		if (assetUUID != 0) {
			s_Lookup[LookupKey{ assetUUID, QuantizePixelSize(pixelSize) }] = idx;
		}
		return h;
	}

	FontHandle FontManager::FindExisting(uint64_t assetUUID, float pixelSize) {
		auto it = s_Lookup.find(LookupKey{ assetUUID, QuantizePixelSize(pixelSize) });
		if (it == s_Lookup.end()) return FontHandle::Invalid();
		const uint16_t idx = it->second;
		if (idx >= s_Slots.size() || !s_Slots[idx].InUse) {
			return FontHandle::Invalid();
		}
		return FontHandle{ idx, s_Slots[idx].Generation };
	}

} // namespace Axiom
