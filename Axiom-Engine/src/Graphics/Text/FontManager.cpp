#include "pch.hpp"
#include "Graphics/Text/FontManager.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Serialization/Path.hpp"

#include <cmath>
#include <filesystem>

namespace Axiom {
    namespace {
        // Quantize pixel size to nearest int for cache key. Two requests
        // for 31.7 px and 32.0 px share the same atlas; sub-pixel size
        // hinting is not part of the MVP.
        int QuantizePixelSize(float pixelSize) {
            return std::max(1, static_cast<int>(std::lround(pixelSize)));
        }
    }

    bool FontManager::Initialize() {
        if (s_IsInitialized) {
            AIM_CORE_WARN("FontManager already initialized");
            return true;
        }
        s_Slots.clear();
        s_Lookup.clear();
        s_DefaultFont = FontHandle::Invalid();
        s_IsInitialized = true;

        // Register the engine-shipped default font as a built-in asset so
        // it shows up in the inspector's Font picker even for projects
        // that have no .ttf in their own Assets folder. Its absolute path
        // depends on where the executable runs from, so we resolve it
        // here once at engine init.
        std::string fontDir = Path::ResolveAxiomAssets("Fonts");
        if (fontDir.empty()) {
            fontDir = Path::Combine(Path::ExecutableDir(), "AxiomAssets", "Fonts");
        }
        const std::string defaultFontPath = Path::Combine(fontDir, "DefaultSans-Regular.ttf");
        if (std::filesystem::exists(defaultFontPath)) {
            std::error_code ec;
            const std::string canonical = std::filesystem::weakly_canonical(
                std::filesystem::path(defaultFontPath), ec).make_preferred().string();
            const std::string resolved = ec ? defaultFontPath : canonical;
            AssetRegistry::RegisterBuiltInAsset(resolved, k_DefaultFontAssetId, AssetKind::Font);
            AIM_CORE_INFO_TAG("FontManager", "Default font registered as built-in: {}", resolved);
        } else {
            AIM_CORE_WARN_TAG("FontManager", "Default font missing at {} - text components will fall back to invalid", defaultFontPath);
        }
        return true;
    }

    void FontManager::Shutdown() {
        if (!s_IsInitialized) {
            return;
        }
        UnloadAll();
        s_DefaultFont = FontHandle::Invalid();
        s_IsInitialized = false;
    }

    FontHandle FontManager::LoadFont(std::string_view path, float pixelSize) {
        if (!s_IsInitialized || path.empty() || pixelSize <= 0.0f) {
            return FontHandle::Invalid();
        }

        // Resolve path through the asset registry so we cache by stable
        // GUID, not by the (potentially relative) string the caller passed.
        const std::string pathStr(path);
        const uint64_t uuid = AssetRegistry::GetOrCreateAssetUUID(pathStr);

        if (uuid != 0) {
            FontHandle existing = FindExisting(uuid, pixelSize);
            if (existing.IsValid()) {
                return existing;
            }
        }

        auto font = std::make_unique<Font>();
        if (!font->LoadFromFile(pathStr, pixelSize)) {
            return FontHandle::Invalid();
        }

        return CreateSlot(std::move(font), uuid, pixelSize);
    }

    FontHandle FontManager::LoadFontByUUID(uint64_t assetId, float pixelSize) {
        if (!s_IsInitialized || assetId == 0 || pixelSize <= 0.0f) {
            return FontHandle::Invalid();
        }
        if (!AssetRegistry::IsFont(assetId)) {
            return FontHandle::Invalid();
        }

        FontHandle existing = FindExisting(assetId, pixelSize);
        if (existing.IsValid()) {
            return existing;
        }

        const std::string path = AssetRegistry::ResolvePath(assetId);
        if (path.empty()) {
            return FontHandle::Invalid();
        }

        auto font = std::make_unique<Font>();
        if (!font->LoadFromFile(path, pixelSize)) {
            return FontHandle::Invalid();
        }

        return CreateSlot(std::move(font), assetId, pixelSize);
    }

    void FontManager::UnloadFont(const FontHandle& handle) {
        if (!IsValid(handle)) {
            return;
        }
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
        // Bump generation so old handles compare invalid.
        ++slot.Generation;
    }

    void FontManager::UnloadAll() {
        for (size_t i = 0; i < s_Slots.size(); ++i) {
            Slot& slot = s_Slots[i];
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
        if (!handle.IsValid() || handle.index >= s_Slots.size()) {
            return false;
        }
        const Slot& slot = s_Slots[handle.index];
        return slot.InUse && slot.Generation == handle.generation && slot.Font && slot.Font->IsLoaded();
    }

    Font* FontManager::GetFont(const FontHandle& handle) {
        if (!IsValid(handle)) {
            return nullptr;
        }
        return s_Slots[handle.index].Font.get();
    }

    uint64_t FontManager::GetFontAssetUUID(const FontHandle& handle) {
        if (!IsValid(handle)) {
            return 0;
        }
        return s_Slots[handle.index].AssetUUID;
    }

    FontHandle FontManager::GetDefaultFont() {
        if (IsValid(s_DefaultFont)) {
            return s_DefaultFont;
        }
        if (!s_IsInitialized) {
            return FontHandle::Invalid();
        }

        // Goes through the same UUID-based path as user-picked fonts so
        // the FontManager cache de-duplicates across both surfaces.
        // Initialize() already registered k_DefaultFontAssetId with the
        // engine-bundled .ttf path.
        s_DefaultFont = LoadFontByUUID(k_DefaultFontAssetId, 32.0f);
        if (!IsValid(s_DefaultFont)) {
            AIM_CORE_WARN_TAG("FontManager",
                "Default font could not be loaded - check that AxiomAssets/Fonts/DefaultSans-Regular.ttf exists next to the executable.");
        }
        return s_DefaultFont;
    }

    FontHandle FontManager::CreateSlot(std::unique_ptr<Font> font, uint64_t assetUUID, float pixelSize) {
        // First reuse a free slot if any; otherwise grow.
        for (size_t i = 0; i < s_Slots.size(); ++i) {
            Slot& slot = s_Slots[i];
            if (!slot.InUse) {
                slot.Font = std::move(font);
                slot.AssetUUID = assetUUID;
                slot.PixelSize = pixelSize;
                slot.InUse = true;
                ++slot.Generation;
                FontHandle handle{ static_cast<uint16_t>(i), slot.Generation };
                if (assetUUID != 0) {
                    s_Lookup[LookupKey{ assetUUID, QuantizePixelSize(pixelSize) }] = handle.index;
                }
                return handle;
            }
        }

        if (s_Slots.size() >= FontHandle::k_InvalidIndex) {
            AIM_CORE_ERROR_TAG("FontManager", "Font slot table exhausted ({} slots)", s_Slots.size());
            return FontHandle::Invalid();
        }

        Slot fresh;
        fresh.Font = std::move(font);
        fresh.AssetUUID = assetUUID;
        fresh.PixelSize = pixelSize;
        fresh.InUse = true;
        fresh.Generation = 1;
        s_Slots.push_back(std::move(fresh));
        FontHandle handle{ static_cast<uint16_t>(s_Slots.size() - 1), 1 };
        if (assetUUID != 0) {
            s_Lookup[LookupKey{ assetUUID, QuantizePixelSize(pixelSize) }] = handle.index;
        }
        return handle;
    }

    FontHandle FontManager::FindExisting(uint64_t assetUUID, float pixelSize) {
        if (assetUUID == 0) {
            return FontHandle::Invalid();
        }
        const LookupKey key{ assetUUID, QuantizePixelSize(pixelSize) };
        auto it = s_Lookup.find(key);
        if (it == s_Lookup.end()) {
            return FontHandle::Invalid();
        }
        const uint16_t idx = it->second;
        if (idx >= s_Slots.size()) {
            return FontHandle::Invalid();
        }
        const Slot& slot = s_Slots[idx];
        if (!slot.InUse || !slot.Font || !slot.Font->IsLoaded()) {
            return FontHandle::Invalid();
        }
        return FontHandle{ idx, slot.Generation };
    }

}
