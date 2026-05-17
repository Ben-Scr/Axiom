#pragma once
#include "Core/Export.hpp"
#include "Graphics/Shader.hpp"
#include "Graphics/ShaderEntry.hpp"
#include "Graphics/ShaderHandle.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <vector>

namespace Index {

    // ShaderManager — slot-table cache for compiled Shader programs. Mirrors
    // TextureManager/AudioManager's pattern: generational handles, dedup by
    // the (vsPath, fsPath) pair (a shader program is the linked pair, so the
    // same vs paired with a different fs is a different program), and a
    // PurgeUnreferenced hook that frees entries not held by any registered
    // ReferenceProvider.
    //
    // No ECS scan today: no component holds a ShaderHandle. When Material or
    // a similar component lands, extend PurgeUnreferenced to walk scenes
    // alongside the provider sweep.
    class INDEX_API ShaderManager {
    public:
        static void Initialize();
        static void Shutdown();
        static bool IsInitialized() { return s_IsInitialized; }

        // Load a shader program from a vertex + fragment source pair. If the
        // same (vsPath, fsPath) is already cached, returns the existing
        // handle. Returns an invalid handle on compile/link failure (the
        // cache is not poisoned by failed loads).
        static ShaderHandle LoadShader(const std::string_view& vsPath, const std::string_view& fsPath);

        // Resolve both UUIDs via AssetRegistry and forward to LoadShader.
        static ShaderHandle LoadShaderByUUID(uint64_t vsAssetId, uint64_t fsAssetId);

        static void UnloadShader(ShaderHandle handle);
        static Shader* GetShader(ShaderHandle handle);
        static bool IsValid(ShaderHandle handle);
        static ShaderHandle GetShaderHandle(const std::string& vsPath, const std::string& fsPath);

        // Rebuild the underlying Shader from its stored paths. On failure,
        // the existing shader is left in place so renderers don't break
        // mid-frame on a typo in a hot-reloaded .wgsl. Returns true on
        // successful rebuild.
        static bool ReloadShader(ShaderHandle handle);

        // Reload every entry whose vs OR fs path matches (canonicalized).
        // Returns the count of entries successfully reloaded.
        static size_t ReloadShaderPath(const std::string& path);

        // Reload every cached shader from disk. Used by the editor on a
        // manual "Reload shaders" command.
        static size_t ReloadShadersFromDisk();

        static std::vector<ShaderHandle> GetLoadedHandles();
        static void UnloadAll();

        // Walks every registered ReferenceProvider, building the set of
        // handles still in use. Frees every valid entry not in that set.
        // Returns the number of entries freed. Safe to call between frames.
        //
        // TODO: when a Material component (or any other ECS holder of
        // ShaderHandle) is added, extend this to walk every live scene's
        // registry — see TextureManager::PurgeUnreferenced.
        static size_t PurgeUnreferenced();

        using ReferenceEmitter = std::function<void(ShaderHandle)>;
        using ReferenceProvider = std::function<void(const ReferenceEmitter&)>;
        static uint32_t AddReferenceProvider(ReferenceProvider provider);
        static void RemoveReferenceProvider(uint32_t token);

    private:
        static ShaderHandle FindShaderByPaths(const std::string& vsPath, const std::string& fsPath);

        static std::vector<ShaderEntry> s_Shaders;
        static std::queue<uint16_t> s_FreeIndices;
        static bool s_IsInitialized;
    };

} // namespace Index
