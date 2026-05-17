#include "pch.hpp"
#include "Graphics/TextureManager.hpp"

#include "Assets/AssetRegistry.hpp"
#include "Core/Log.hpp"
#include "Graphics/TextureEntry.hpp"
#include "Serialization/Path.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <utility>

// =============================================================================
// TextureManager — slot table + handle-generation pool.
// -----------------------------------------------------------------------------
// Texture loading routes through Texture2D::Load() (decoded via stbi_load +
// uploaded via the active backend), so callers get a real GPU resource per
// LoadTexture/LoadTextureByUUID call.
//
// Default textures (Square / Pixel / Circle / etc.) are pre-loaded from
// IndexAssets/Textures/Default/*.png at Initialize. Sprite + UI fallback
// paths look these up via GetDefaultTexture so a SpriteRendererComponent
// or ImageComponent with no user-assigned texture still produces a visible
// white quad.
// =============================================================================

namespace Index {

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

		struct DestroyListenerEntry {
			uint32_t Token = 0;
			TextureManager::DestroyListener Listener;
		};
		std::vector<DestroyListenerEntry> g_DestroyListeners;
		uint32_t g_NextDestroyListenerToken = 1;

		void FireDestroyListeners(TextureHandle handle) {
			// Iterate over a local copy — listeners are allowed to remove
			// themselves from inside the callback. This is rare (registration
			// is normally lifecycle-bound), but copy cost is tiny vs. the
			// safety guarantee.
			if (g_DestroyListeners.empty()) return;
			std::vector<DestroyListenerEntry> snapshot = g_DestroyListeners;
			for (const auto& e : snapshot) {
				if (e.Listener) e.Listener(handle);
			}
		}

		// Resolved handles for each DefaultTexture enum value. Indexed
		// directly by the enum's underlying byte. Held alongside (not
		// inside) s_DefaultTextures because s_DefaultTextures stores
		// the on-disk paths for diagnostics — we want both.
		std::array<TextureHandle, 9> g_DefaultHandles{};

		// Reverse index from a (path, filter, wrap-u, wrap-v) tuple-hash to
		// the slot in s_Textures. Built incrementally on insert / cleared
		// on UnloadTexture so FindTextureByPath becomes O(1) average instead
		// of an O(N) linear scan over s_Textures. The previous linear scan
		// was hot during sprite load + every FileWatcher hot-reload event.
		// 0 is a valid slot index, so we store (index+1) and use 0 as "miss".
		std::unordered_map<std::uint64_t, std::uint32_t> g_PathIndex;

		// Per-slot canonical key, computed once at insert time and reused on
		// hot-reload comparisons. Previously ReloadTexturePath called
		// std::filesystem::weakly_canonical for every loaded texture on every
		// event — that's N stat() syscalls per FileWatcher tick. Storing the
		// canonical form here cuts hot-reload to one canonicalization for
		// the incoming event path plus pure string compares against the
		// stored keys.
		std::vector<std::string> g_CanonicalKeys;

		std::string ToLower(std::string value) {
			std::transform(value.begin(), value.end(), value.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		bool IsReloadableImagePath(const std::filesystem::path& path) {
			const std::string ext = ToLower(path.extension().string());
			return ext == ".png" || ext == ".jpg" || ext == ".jpeg"
				|| ext == ".bmp" || ext == ".tga";
		}

		std::string CanonicalPathKey(const std::string& path) {
			std::error_code ec;
			std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
			if (ec) {
				canonical = std::filesystem::absolute(path, ec);
				if (ec) {
					canonical = path;
				}
			}
			return canonical.lexically_normal().make_preferred().string();
		}

		// FNV-1a 64-bit over the path string then mixed with the sampler
		// state. Keeps the lookup key stable across the lifetime of a slot
		// (the path string doesn't move once stored).
		std::uint64_t HashLookupKey(std::string_view path,
			Filter filter,
			Wrap u,
			Wrap v)
		{
			std::uint64_t h = 0xcbf29ce484222325ULL;
			for (char c : path) {
				h ^= static_cast<std::uint8_t>(c);
				h *= 0x100000001b3ULL;
			}
			h ^= static_cast<std::uint64_t>(filter) << 1;
			h ^= static_cast<std::uint64_t>(u) << 16;
			h ^= static_cast<std::uint64_t>(v) << 32;
			h *= 0x100000001b3ULL;
			return h;
		}

		void EnsureCanonicalKeyCapacity(std::size_t slotCount) {
			if (g_CanonicalKeys.size() < slotCount) {
				g_CanonicalKeys.resize(slotCount);
			}
		}
	}

	void TextureManager::Initialize() {
		if (s_IsInitialized) return;
		s_Textures.clear();
		while (!s_FreeIndices.empty()) s_FreeIndices.pop();
		s_Textures.reserve(64);
		g_PathIndex.clear();
		g_PathIndex.reserve(64);
		g_CanonicalKeys.clear();
		g_CanonicalKeys.reserve(64);
		g_DefaultHandles.fill(TextureHandle{});
		s_IsInitialized = true;
		LoadDefaultTextures();
	}

	void TextureManager::Shutdown() {
		UnloadAll(true);
		s_Textures.clear();
		while (!s_FreeIndices.empty()) s_FreeIndices.pop();
		g_Providers.clear();
		g_DestroyListeners.clear();
		g_PathIndex.clear();
		g_CanonicalKeys.clear();
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
		// flipVertical=false: WebGPU's texture-coordinate origin is
		// top-left (matching stb_image's default decode order), and the
		// sprite shader's UV calc (`0.5 - in.position.y` in Shader.cpp
		// k_SpriteWGSL) already maps quad-up to texture-row-0. Loading
		// with stb's vertical flip on top of that produced a net flip
		// — sprites and UI Image components rendered upside-down. This
		// path is the OpenGL-legacy default that the WebGPU port
		// inherited; turning it off makes textures display right-side-
		// up without touching the shader.
		if (!slot.Texture.Load(pathStr.c_str(), /*generateMipmaps=*/true,
			/*srgb=*/false, /*flipVertical=*/false))
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
		// Register in the reverse index so subsequent FindTextureByPath /
		// LoadTexture calls hit O(1) instead of walking s_Textures. Stored
		// as (idx + 1) so 0 keeps its "no entry" meaning.
		g_PathIndex[HashLookupKey(pathStr, filter, u, v)] =
			static_cast<std::uint32_t>(idx) + 1u;
		EnsureCanonicalKeyCapacity(s_Textures.size());
		g_CanonicalKeys[idx] = CanonicalPathKey(pathStr);
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
		// Fire listeners BEFORE we tear the slot down so subscribers (e.g.
		// Renderer2D's bind-group cache) can still resolve handle -> Texture2D
		// to capture the soon-to-be-stale GPU pool ID.
		FireDestroyListeners(handle);
		TextureEntry& slot = s_Textures[handle.index];
		// Drop the reverse-index entry BEFORE we clear the name — otherwise
		// the hash key derived from slot.Name would no longer match what
		// was inserted. Generation bump below ensures stale handles to this
		// slot fail IsValid even if the slot gets recycled with a new path.
		g_PathIndex.erase(HashLookupKey(slot.Name, slot.SamplerFilter, slot.WrapU, slot.WrapV));
		if (handle.index < g_CanonicalKeys.size()) {
			g_CanonicalKeys[handle.index].clear();
		}
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

	bool TextureManager::ReloadTexture(TextureHandle handle) {
		if (!IsValid(handle)) return false;

		TextureEntry& slot = s_Textures[handle.index];
		if (slot.Name.empty() || !std::filesystem::exists(slot.Name) || !IsReloadableImagePath(slot.Name)) {
			return false;
		}

		Texture2D reloaded;
		if (!reloaded.Load(slot.Name.c_str(), /*generateMipmaps=*/true,
			/*srgb=*/false, /*flipVertical=*/false))
		{
			IDX_CORE_WARN_TAG("TextureManager", "Hot reload failed for texture: {}", slot.Name);
			return false;
		}

		reloaded.SetSampler(slot.SamplerFilter, slot.WrapU, slot.WrapV);
		// Notify listeners BEFORE the move-assign destroys the previous
		// GPU resource — the renderer's bind-group cache is keyed off the
		// old Texture2D::GetHandle() pool ID and must evict it now.
		FireDestroyListeners(handle);
		slot.Texture = std::move(reloaded);
		IDX_CORE_INFO_TAG("TextureManager", "Reloaded texture: {}", slot.Name);
		return true;
	}

	size_t TextureManager::ReloadTexturePath(const std::string& path) {
		if (!s_IsInitialized || path.empty()) return 0;
		const std::string targetKey = CanonicalPathKey(path);
		size_t count = 0;
		// Compare against the pre-canonicalized keys stored at insert time
		// instead of re-canonicalizing every slot's stored name. Each
		// CanonicalPathKey call does a weakly_canonical() — i.e. a stat()
		// syscall — so the previous N×canonicalize per FileWatcher event
		// scaled poorly with project size.
		const std::size_t end = std::min<std::size_t>(s_Textures.size(), g_CanonicalKeys.size());
		for (size_t i = 0; i < end; ++i) {
			const TextureEntry& slot = s_Textures[i];
			if (!slot.IsValid || slot.Name.empty()) continue;
			if (g_CanonicalKeys[i] == targetKey) {
				if (ReloadTexture(TextureHandle{ static_cast<uint16_t>(i), slot.Generation })) {
					++count;
				}
			}
		}
		return count;
	}

	size_t TextureManager::ReloadTexturesFromDisk() {
		if (!s_IsInitialized) return 0;
		size_t count = 0;
		for (size_t i = 0; i < s_Textures.size(); ++i) {
			const TextureEntry& slot = s_Textures[i];
			if (!slot.IsValid || slot.Name.empty() || !IsReloadableImagePath(slot.Name)) {
				continue;
			}
			if (ReloadTexture(TextureHandle{ static_cast<uint16_t>(i), slot.Generation })) {
				++count;
			}
		}
		return count;
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
				// Fire per-slot BEFORE the slot is invalidated so listeners
				// can still resolve handle -> GPU pool ID.
				FireDestroyListeners(TextureHandle{ static_cast<uint16_t>(i), slot.Generation });
				slot.Texture.Destroy();
				slot.Name.clear();
				slot.IsValid = false;
				++slot.Generation;
				s_FreeIndices.push(static_cast<uint16_t>(i));
			}
		}
		// Reverse index is keyed off names that no longer exist; clearing
		// it wholesale matches the old behaviour but avoids leaving stale
		// hash entries that would resolve to recycled-but-different slots.
		g_PathIndex.clear();
		for (std::string& key : g_CanonicalKeys) {
			key.clear();
		}
	}

	uint64_t TextureManager::GetTextureAssetUUID(TextureHandle handle) {
		if (!IsValid(handle)) return 0;
		return AssetRegistry::GetOrCreateAssetUUID(s_Textures[handle.index].Name);
	}

	size_t TextureManager::PurgeUnreferenced() {
		// Trust the registered providers + scene scan: every entry not
		// held by a live ECS component or a registered provider gets
		// evicted.
		// Real impl walks every loaded scene; we approximate by having
		// every provider opt in to keeping its handles alive.
		// TODO: walk every loaded scene to drop stale handles.
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

	uint32_t TextureManager::AddDestroyListener(DestroyListener listener) {
		if (!listener) return 0;
		const uint32_t token = g_NextDestroyListenerToken++;
		g_DestroyListeners.push_back({ token, std::move(listener) });
		return token;
	}

	void TextureManager::RemoveDestroyListener(uint32_t token) {
		auto it = std::remove_if(g_DestroyListeners.begin(), g_DestroyListeners.end(),
			[token](const DestroyListenerEntry& e) { return e.Token == token; });
		g_DestroyListeners.erase(it, g_DestroyListeners.end());
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
		// O(1) hash lookup against the reverse index. The previous linear
		// scan over s_Textures fired on every LoadTexture call (for dedup)
		// and dominated repeated sprite loads in scenes with many textures.
		auto it = g_PathIndex.find(HashLookupKey(path, filter, u, v));
		if (it == g_PathIndex.end() || it->second == 0u) {
			return TextureHandle{};
		}
		const std::uint32_t slotIdx = it->second - 1u;
		if (slotIdx >= s_Textures.size()) {
			return TextureHandle{};
		}
		const TextureEntry& e = s_Textures[slotIdx];
		// Defensive verify against hash collision — exceedingly rare with
		// FNV-1a but cheap to confirm before handing back a handle.
		if (!e.IsValid || e.Name != path
			|| e.SamplerFilter != filter
			|| e.WrapU != u || e.WrapV != v)
		{
			return TextureHandle{};
		}
		return TextureHandle{ static_cast<uint16_t>(slotIdx), e.Generation };
	}

	TextureHandle TextureManager::FindTextureByPath(const std::string& path) {
		// Sampler-agnostic lookup falls back to a scan — callers that want
		// O(1) should supply the sampler tuple. This overload is used by
		// editor / introspection paths, not hot-path renderer code.
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
		// IndexAssets/Textures/Default/. The order here MUST match the
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

		const std::string assetsRoot = Path::ResolveIndexAssets("");
		if (assetsRoot.empty()) {
			IDX_CORE_WARN_TAG("TextureManager",
				"IndexAssets root not resolved — default textures unavailable; "
				"sprites/UI without explicit textures will render invisible.");
			return;
		}

		for (size_t i = 0; i < g_DefaultHandles.size(); ++i) {
			const std::string fullPath = Path::Combine(assetsRoot, k_DefaultPaths[i]);
			if (!std::filesystem::exists(fullPath)) {
				IDX_CORE_WARN_TAG("TextureManager",
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
				IDX_CORE_WARN_TAG("TextureManager",
					"Default texture failed to load: {}", fullPath);
				continue;
			}
			g_DefaultHandles[i] = h;
			s_DefaultTextures[i] = fullPath;
		}

		IDX_CORE_INFO_TAG("TextureManager",
			"Loaded default textures from {}", assetsRoot);
	}

} // namespace Index
