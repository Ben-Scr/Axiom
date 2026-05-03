#pragma once
#include "Core/Export.hpp"
#include "Graphics/DefaultTexture.hpp"
#include "Graphics/Texture2D.hpp"
#include "Graphics/TextureEntry.hpp"
#include "TextureHandle.hpp"
#include "Serialization/Path.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace Axiom {
    class Application;
}

namespace Axiom {
        class AXIOM_API TextureManager {
        public:
            static void Initialize();
            static void Shutdown();

            static TextureHandle LoadTexture(const std::string_view& path, Filter filter = Filter::Point, Wrap u = Wrap::Clamp, Wrap v = Wrap::Clamp);
            static TextureHandle LoadTextureByUUID(uint64_t assetId, Filter filter = Filter::Point, Wrap u = Wrap::Clamp, Wrap v = Wrap::Clamp);
            static TextureHandle GetDefaultTexture(DefaultTexture type);
            static void UnloadTexture(TextureHandle handle);
            // Look up an already-loaded texture by name. The filter/wrap
            // parameters disambiguate same-named textures loaded with
            // different sampler settings (the same path can legitimately
            // appear multiple times in s_Textures with distinct keys). For
            // legacy callers, the 1-arg overload returns the first match
            // ignoring sampler settings — prefer the keyed form for correctness.
            static TextureHandle GetTextureHandle(const std::string& name, Filter filter, Wrap u = Wrap::Clamp, Wrap v = Wrap::Clamp);
            [[deprecated("Sampler-agnostic lookup is ambiguous when the same path is loaded with multiple Filter/Wrap combinations. Pass filter+wrap explicitly.")]]
            static TextureHandle GetTextureHandle(const std::string& name);
            static Texture2D* GetTexture(TextureHandle handle);
            static std::vector<TextureHandle> GetLoadedHandles();
            static void UnloadAll(bool defaultTextures = false);
            static uint64_t GetTextureAssetUUID(TextureHandle handle);

            // Walks every live Scene's registry collecting TextureHandle values from
            // components that hold them (SpriteRendererComponent, ParticleSystem2DComponent,
            // any others with a TextureHandle field). Frees every entry not in that set.
            // Default/built-in textures (the entries reserved at Initialize() time) are
            // never evicted. Safe to call between frames; may take O(textures × components)
            // time. Returns the number of entries freed.
            //
            // Before scanning scenes, this also asks every registered external
            // reference provider (see AddReferenceProvider) to report the
            // handles it holds. Without this hook, the editor's
            // ThumbnailCache, AssetBrowser previews, EditorIcons, and any
            // package that holds TextureHandles outside the ECS would have
            // their references silently invalidated on a Purge call.
            static size_t PurgeUnreferenced();

            // Register a callback that emits the TextureHandles its caller
            // currently holds. Used by the editor and by packages to opt
            // their non-ECS handles into PurgeUnreferenced. Returns a token
            // that must be passed to RemoveReferenceProvider to unregister.
            using ReferenceEmitter = std::function<void(TextureHandle)>;
            using ReferenceProvider = std::function<void(const ReferenceEmitter&)>;
            static uint32_t AddReferenceProvider(ReferenceProvider provider);
            static void RemoveReferenceProvider(uint32_t token);

            /// Returns the texture path relative to a texture root directory.
            /// This is the same format accepted by LoadTexture().
            static std::string GetTextureName(TextureHandle handle) {
                if (handle.index >= s_Textures.size() || !s_Textures[handle.index].IsValid
                    || s_Textures[handle.index].Generation != handle.generation)
                    return "";

                const std::string& fullName = s_Textures[handle.index].Name;

                // Try stripping any known texture root prefix
                auto tryStrip = [&](const std::string& root) -> std::string {
                    if (root.empty() || fullName.size() <= root.size()) return "";
                    if (fullName.compare(0, root.size(), root) != 0) return "";
                    size_t start = root.size();
                    if (start < fullName.size() && (fullName[start] == '/' || fullName[start] == '\\'))
                        start++;
                    return fullName.substr(start);
                };

                // Try primary root first
                std::string rel = tryStrip(s_RootPath);
                if (!rel.empty()) return rel;

                // Try user Assets/Textures as fallback
                std::string base = Path::ExecutableDir();
                rel = tryStrip(Path::Combine(base, "Assets", "Textures"));
                if (!rel.empty()) return rel;

                return fullName;
            }

            static bool IsValid(TextureHandle handle) {
                return handle.index < s_Textures.size() &&
                    s_Textures[handle.index].IsValid &&
                    s_Textures[handle.index].Generation == handle.generation;
            }

            // Sum of all currently-loaded textures' GPU memory footprint, in
            // bytes. Estimate: 4 bytes per pixel (RGBA8). Used by the profiler's
            // "Texture Memory" module. Default textures are excluded — they're
            // tiny (1x1 / 32x32) and aren't user-controlled state.
            static std::size_t GetTotalTextureMemoryBytes();

		private:
			static TextureHandle FindTextureByPath(const std::string& path, Filter filter, Wrap u, Wrap v);
			static TextureHandle FindTextureByPath(const std::string& path);
			static void LoadDefaultTextures();

            static std::array<std::string, 9> s_DefaultTextures;
            static std::vector<TextureEntry> s_Textures;
            static std::queue<uint16_t> s_FreeIndices;
            static bool s_IsInitialized;

            static std::string s_RootPath;

            friend class Renderer2D;
        };
}

namespace std {
    template<>
    struct hash<Axiom::TextureHandle> {
        size_t operator()(const Axiom::TextureHandle& h) const noexcept {
            return (static_cast<size_t>(h.index) << 16) ^ static_cast<size_t>(h.generation);
        }
    };
}
