#include "pch.hpp"
#include "Graphics/TextureManager.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Core/Log.hpp"
#include "Graphics/TextureEntry.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

// =============================================================================
// TextureManager — real bgfx implementation.
// -----------------------------------------------------------------------------
// Real slot table + handle-generation pool, mirroring the OpenGL impl's
// shape. Texture loading routes through Texture2D's bgfx Load() (decoded
// via stbi_load + uploaded via bgfx::createTexture2D), so callers get a
// real GPU resource per LoadTexture/LoadTextureByUUID call.
//
// Default textures (Square / Pixel / Circle / etc.) are pre-loaded from
// AxiomAssets/Textures/Default/*.png at Initialize. Sprite + UI fallback
// paths look these up via GetDefaultTexture so a SpriteRendererComponent
// or ImageComponent with no user-assigned texture still produces a
// visible white quad — Stage 2.1's "render nothing instead of magenta"
// behaviour broke even the simplest scenes (a default Square + Image
// pair would render blank because bgfx samples zero from an unbound
// s_albedo, multiplied by the per-instance color = transparent black).
// =============================================================================

namespace Axiom {

	std::array<std::string, 9> TextureManager::s_DefaultTextures{};
	std::vector<TextureEntry>  TextureManager::s_Textures;
	std::queue<uint16_t>       TextureManager::s_FreeIndices;
	bool                       TextureManager::s_IsInitialized = false;
	std::string                TextureManager::s_RootPath;

	namespace {
		struct ProviderEntry {
			uint32_t Token = 0;
			TextureManager::ReferenceProvider Provider;
		};
		std::vector<ProviderEntry> g_Providers;
		uint32_t g_NextProviderToken = 1;

		// Resolved handles for each DefaultTexture enum value. Indexed
		// directly by the enum's underlying byte. Held alongside (not
		// inside) s_DefaultTextures because s_DefaultTextures stores
		// the on-disk paths for diagnostics — we want both.
		std::array<TextureHandle, 9> g_DefaultHandles{};
	}

	void TextureManager::Initialize() {
		if (s_IsInitialized) return;
		s_Textures.clear();
		while (!s_FreeIndices.empty()) s_FreeIndices.pop();
		s_Textures.reserve(64);
		g_DefaultHandles.fill(TextureHandle{});
		s_IsInitialized = true;
		LoadDefaultTextures();
	}

	void TextureManager::Shutdown() {
		UnloadAll(true);
		s_Textures.clear();
		while (!s_FreeIndices.empty()) s_FreeIndices.pop();
		g_Providers.clear();
		s_IsInitialized = false;
	}

	TextureHandle TextureManager::LoadTexture(const std::string_view& path,
		Filter filter, Wrap u, Wrap v)
	{
		if (!s_IsInitialized || path.empty()) return TextureHandle{};
		const std::string pathStr(path);

		// Reuse existing slot when the same (path, filter, wrap) was
		// already loaded — matches the OpenGL impl's de-dup behaviour.
		TextureHandle existing = FindTextureByPath(pathStr, filter, u, v);
		if (existing.index != 0 || (existing.generation != 0)) {
			// FindTextureByPath returns a default-constructed handle on
			// miss, so distinguish via slot validity:
			if (IsValid(existing)) return existing;
		}

		// Inline slot acquisition — accessing the private statics from
		// inside a TextureManager:: member rather than a free function.
		uint16_t idx;
		if (!s_FreeIndices.empty()) {
			idx = s_FreeIndices.front();
			s_FreeIndices.pop();
		}
		else {
			idx = static_cast<uint16_t>(s_Textures.size());
			s_Textures.emplace_back();
		}
		TextureEntry& slot = s_Textures[idx];
		if (!slot.Texture.Load(pathStr.c_str(), /*generateMipmaps=*/true,
			/*srgb=*/false, /*flipVertical=*/true))
		{
			s_FreeIndices.push(idx);
			return TextureHandle{};
		}
		slot.Texture.SetSampler(filter, u, v);
		slot.Name = pathStr;
		slot.SamplerFilter = filter;
		slot.WrapU = u;
		slot.WrapV = v;
		slot.IsValid = true;
		return TextureHandle{ idx, slot.Generation };
	}

	TextureHandle TextureManager::LoadTextureByUUID(uint64_t assetId,
		Filter filter, Wrap u, Wrap v)
	{
		if (!s_IsInitialized || assetId == 0) return TextureHandle{};
		std::string path = AssetRegistry::ResolvePath(assetId);
		if (path.empty()) {
			AssetRegistry::MarkDirty();
			AssetRegistry::Sync();
			path = AssetRegistry::ResolvePath(assetId);
		}
		if (path.empty()) return TextureHandle{};
		return LoadTexture(path, filter, u, v);
	}

	TextureHandle TextureManager::GetDefaultTexture(DefaultTexture type) {
		const size_t idx = static_cast<size_t>(type);
		if (idx >= g_DefaultHandles.size()) return TextureHandle{};
		return g_DefaultHandles[idx];
	}

	void TextureManager::UnloadTexture(TextureHandle handle) {
		if (!IsValid(handle)) return;
		TextureEntry& slot = s_Textures[handle.index];
		slot.Texture.Destroy();
		slot.Name.clear();
		slot.IsValid = false;
		++slot.Generation;
		s_FreeIndices.push(handle.index);
	}

	TextureHandle TextureManager::GetTextureHandle(const std::string& name,
		Filter filter, Wrap u, Wrap v)
	{
		return FindTextureByPath(name, filter, u, v);
	}

	TextureHandle TextureManager::GetTextureHandle(const std::string& name) {
		return FindTextureByPath(name);
	}

	Texture2D* TextureManager::GetTexture(TextureHandle handle) {
		if (!IsValid(handle)) return nullptr;
		return &s_Textures[handle.index].Texture;
	}

	std::vector<TextureHandle> TextureManager::GetLoadedHandles() {
		std::vector<TextureHandle> out;
		out.reserve(s_Textures.size());
		for (size_t i = 0; i < s_Textures.size(); ++i) {
			if (s_Textures[i].IsValid) {
				out.push_back(TextureHandle{ static_cast<uint16_t>(i), s_Textures[i].Generation });
			}
		}
		return out;
	}

	void TextureManager::UnloadAll(bool /*defaultTextures*/) {
		for (size_t i = 0; i < s_Textures.size(); ++i) {
			TextureEntry& slot = s_Textures[i];
			if (slot.IsValid) {
				slot.Texture.Destroy();
				slot.Name.clear();
				slot.IsValid = false;
				++slot.Generation;
				s_FreeIndices.push(static_cast<uint16_t>(i));
			}
		}
	}

	uint64_t TextureManager::GetTextureAssetUUID(TextureHandle handle) {
		if (!IsValid(handle)) return 0;
		return AssetRegistry::GetOrCreateAssetUUID(s_Textures[handle.index].Name);
	}

	size_t TextureManager::PurgeUnreferenced() {
		// Stage 2.1: trust the registered providers + scene scan. Without
		// the default-texture exemption (we don't load defaults under
		// bgfx yet), every entry not held by a live ECS component or a
		// registered provider gets evicted.
		// Real impl walks every loaded scene; we approximate by having
		// every provider opt in to keeping its handles alive.
		// TODO(bgfx-stage-2-followup): mirror the OpenGL impl's scene walk.
		return 0;
	}

	uint32_t TextureManager::AddReferenceProvider(ReferenceProvider provider) {
		if (!provider) return 0;
		const uint32_t token = g_NextProviderToken++;
		g_Providers.push_back({ token, std::move(provider) });
		return token;
	}

	void TextureManager::RemoveReferenceProvider(uint32_t token) {
		auto it = std::remove_if(g_Providers.begin(), g_Providers.end(),
			[token](const ProviderEntry& e) { return e.Token == token; });
		g_Providers.erase(it, g_Providers.end());
	}

	std::size_t TextureManager::GetTotalTextureMemoryBytes() {
		std::size_t total = 0;
		for (const TextureEntry& e : s_Textures) {
			if (!e.IsValid) continue;
			const std::size_t w = static_cast<std::size_t>(e.Texture.GetWidth());
			const std::size_t h = static_cast<std::size_t>(e.Texture.GetHeight());
			total += w * h * 4u; // RGBA8 estimate, same as OpenGL impl.
		}
		return total;
	}

	TextureHandle TextureManager::FindTextureByPath(const std::string& path,
		Filter filter, Wrap u, Wrap v)
	{
		for (size_t i = 0; i < s_Textures.size(); ++i) {
			const TextureEntry& e = s_Textures[i];
			if (!e.IsValid) continue;
			if (e.Name == path
				&& e.SamplerFilter == filter
				&& e.WrapU == u && e.WrapV == v)
			{
				return TextureHandle{ static_cast<uint16_t>(i), e.Generation };
			}
		}
		return TextureHandle{};
	}

	TextureHandle TextureManager::FindTextureByPath(const std::string& path) {
		for (size_t i = 0; i < s_Textures.size(); ++i) {
			const TextureEntry& e = s_Textures[i];
			if (e.IsValid && e.Name == path) {
				return TextureHandle{ static_cast<uint16_t>(i), e.Generation };
			}
		}
		return TextureHandle{};
	}

	void TextureManager::LoadDefaultTextures() {
		// Each enum value in DefaultTexture maps to a PNG shipped under
		// AxiomAssets/Textures/Default/. The order here MUST match the
		// enum declaration in DefaultTexture.hpp — we index by the
		// enum's underlying byte value into g_DefaultHandles.
		static constexpr const char* k_DefaultPaths[9] = {
			"Textures/Default/Square.png",            // Square
			"Textures/Default/Pixel.png",             // Pixel
			"Textures/Default/circle.png",            // Circle
			"Textures/Default/Capsule.png",           // Capsule
			"Textures/Default/IsometricDiamond.png",  // IsometricDiamond
			"Textures/Default/HexagonFlatTop.png",    // HexagonFlatTop
			"Textures/Default/HexagonPointedTop.png", // HexagonPointedTop
			"Textures/Default/9Sliced.png",           // _9Sliced
			"Textures/Default/Invisible.png",         // Invisible
		};

		const std::string assetsRoot = Path::ResolveAxiomAssets("");
		if (assetsRoot.empty()) {
			AIM_CORE_WARN_TAG("TextureManager",
				"AxiomAssets root not resolved — default textures unavailable; "
				"sprites/UI without explicit textures will render invisible.");
			return;
		}

		for (size_t i = 0; i < g_DefaultHandles.size(); ++i) {
			const std::string fullPath = Path::Combine(assetsRoot, k_DefaultPaths[i]);
			if (!std::filesystem::exists(fullPath)) {
				AIM_CORE_WARN_TAG("TextureManager",
					"Default texture missing: {} — sprites/UI relying on this fallback will be invisible.",
					fullPath);
				continue;
			}
			// Wrap::Repeat keeps the legacy OpenGL impl's behaviour:
			// 1×1 white pixels tile harmlessly, and a project-supplied
			// repeating tile texture authored against the engine's
			// "Square" default works without a Wrap-mode override.
			TextureHandle h = LoadTexture(fullPath, Filter::Bilinear, Wrap::Repeat, Wrap::Repeat);
			if (!IsValid(h)) {
				AIM_CORE_WARN_TAG("TextureManager",
					"Default texture failed to load: {}", fullPath);
				continue;
			}
			g_DefaultHandles[i] = h;
			s_DefaultTextures[i] = fullPath;
		}

		AIM_CORE_INFO_TAG("TextureManager",
			"Loaded default textures from {}", assetsRoot);
	}

} // namespace Axiom
