using System;
using System.Runtime.InteropServices;
using System.Threading;
using Axiom.Coroutines;

namespace Axiom.Interop;
/// <summary>
/// Layout must match the C++ ManagedCallbacks struct exactly.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ManagedCallbacksStruct
{
    public delegate* unmanaged<byte*, ulong, int> CreateScriptInstance;
    public delegate* unmanaged<int, void> DestroyScriptInstance;
    public delegate* unmanaged<int, void> InvokeStart;
    public delegate* unmanaged<int, void> InvokeUpdate;
    public delegate* unmanaged<int, void> InvokeOnDestroy;
    public delegate* unmanaged<int, void> InvokeOnEnable;
    public delegate* unmanaged<int, void> InvokeOnDisable;
    public delegate* unmanaged<int, ulong, ulong, ulong, ulong, float, float, void> InvokeCollisionEnter2D;
    public delegate* unmanaged<int, ulong, ulong, ulong, ulong, float, float, void> InvokeCollisionStay2D;
    public delegate* unmanaged<int, ulong, ulong, ulong, ulong, float, float, void> InvokeCollisionExit2D;
    public delegate* unmanaged<byte*, int> ClassExists;
    public delegate* unmanaged<byte*, int> LoadUserAssembly;
    public delegate* unmanaged<void> UnloadUserAssembly;
    public delegate* unmanaged<int, byte*> GetScriptFields;
    public delegate* unmanaged<int, byte*, byte*, void> SetScriptField;
    public delegate* unmanaged<byte*, byte*> GetClassFieldDefs;
    public delegate* unmanaged<void> RaiseApplicationStart;
    public delegate* unmanaged<void> RaiseApplicationPaused;
    public delegate* unmanaged<void> RaiseApplicationQuit;
    public delegate* unmanaged<int, void> RaiseFocusChanged;
    public delegate* unmanaged<int, void> RaiseKeyDown;
    public delegate* unmanaged<int, void> RaiseKeyUp;
    public delegate* unmanaged<int, void> RaiseMouseDown;
    public delegate* unmanaged<int, void> RaiseMouseUp;
    public delegate* unmanaged<float, void> RaiseMouseScroll;
    public delegate* unmanaged<float, float, void> RaiseMouseMove;
    public delegate* unmanaged<byte*, void> RaiseBeforeSceneLoaded;
    public delegate* unmanaged<byte*, void> RaiseSceneLoaded;
    public delegate* unmanaged<byte*, void> RaiseBeforeSceneUnloaded;
    public delegate* unmanaged<byte*, void> RaiseSceneUnloaded;
    public delegate* unmanaged<byte*, byte*, int> CreateGameSystemInstance;
    public delegate* unmanaged<int, void> DestroyGameSystemInstance;
    public delegate* unmanaged<int, void> InvokeGameSystemStart;
    public delegate* unmanaged<int, void> InvokeGameSystemUpdate;
    public delegate* unmanaged<int, void> InvokeGameSystemEnable;
    public delegate* unmanaged<int, void> InvokeGameSystemDisable;
    public delegate* unmanaged<int, void> InvokeGameSystemDestroy;
    public delegate* unmanaged<byte*, int> GameSystemClassExists;
    public delegate* unmanaged<byte*, int> CreateGlobalSystemInstance;
    public delegate* unmanaged<int, void> DestroyGlobalSystemInstance;
    public delegate* unmanaged<int, void> InvokeGlobalSystemInitialize;
    public delegate* unmanaged<int, void> InvokeGlobalSystemUpdate;
    public delegate* unmanaged<int, void> InvokeGlobalSystemEnable;
    public delegate* unmanaged<int, void> InvokeGlobalSystemDisable;
    public delegate* unmanaged<byte*, int> GlobalSystemClassExists;

    // ── New lifecycle slots (appended for binary compat) ──
    public delegate* unmanaged<int, void> InvokeAwake;
    public delegate* unmanaged<int, void> InvokeFixedUpdate;
    public delegate* unmanaged<int, void> InvokeGameSystemAwake;
    public delegate* unmanaged<int, void> InvokeGameSystemFixedUpdate;
    public delegate* unmanaged<int, void> InvokeGlobalSystemFixedUpdate;

    // ── GameSystem field reflection (appended for binary compat) ──
    public delegate* unmanaged<int, byte*> GetGameSystemFields;
    public delegate* unmanaged<int, byte*, byte*, void> SetGameSystemField;

    // ── UI event dispatch (appended for binary compat) ──
    // Native UIEventSystem calls this once per frame after writing
    // its transient flags so the managed UIEventDispatcher can fan
    // out the per-instance UI events (Button.OnClick, Slider.OnValueChanged,
    // etc.) the same frame the engine detected the change.
    public delegate* unmanaged<void> RaiseUiEventDispatch;

    // ── Coroutine pump (appended for binary compat) ──
    // Drains the AxiomSynchronizationContext queue and ticks pending
    // EntityScript coroutine awaiters (frame / seconds / condition).
    // Called from native ScriptSystem::Update at the top of the frame,
    // and ScriptSystem::FixedUpdate for fixed-tick continuations.
    public delegate* unmanaged<float, void> PumpCoroutinesUpdate;
    public delegate* unmanaged<void> PumpCoroutinesFixedUpdate;

    // ── Inspector event bindings (appended for binary compat) ──
    // Order must match ScriptGlue.hpp's ManagedCallbacks struct exactly.
    public delegate* unmanaged<byte*, byte*, int, int> GetInvokableMethodsBuffer;
    // Typed-argument variant: argKind is the byte value of
    // InspectorEventArgKind, argValue is the encoded string per the kind
    // (or null when argKind == 0 / Void).
    public delegate* unmanaged<int, byte*, byte, byte*, int> InvokeScriptMethodByName;
}

/// <summary>
/// Entry point called from C++ via hostfxr load_assembly_and_get_function_pointer.
/// Exchanges native ↔ managed function pointers. Returns 0 on success.
/// </summary>
internal static class ScriptHostBridge
{
    [UnmanagedCallersOnly]
    internal static unsafe int Initialize(
        NativeBindingsStruct* nativeBindings,
        ManagedCallbacksStruct* managedCallbacks)
    {
        try
        {
            NativeCallbacks.SetFrom(nativeBindings);

            managedCallbacks->CreateScriptInstance = &ScriptInstanceManager.CreateScriptInstance;
            managedCallbacks->DestroyScriptInstance = &ScriptInstanceManager.DestroyScriptInstance;
            managedCallbacks->InvokeStart = &ScriptInstanceManager.InvokeStart;
            managedCallbacks->InvokeUpdate = &ScriptInstanceManager.InvokeUpdate;
            managedCallbacks->InvokeOnDestroy = &ScriptInstanceManager.InvokeOnDestroy;
            managedCallbacks->InvokeOnEnable = &ScriptInstanceManager.InvokeOnEnable;
            managedCallbacks->InvokeOnDisable = &ScriptInstanceManager.InvokeOnDisable;
            managedCallbacks->InvokeCollisionEnter2D = &ScriptInstanceManager.InvokeCollisionEnter2D;
            managedCallbacks->InvokeCollisionStay2D = &ScriptInstanceManager.InvokeCollisionStay2D;
            managedCallbacks->InvokeCollisionExit2D = &ScriptInstanceManager.InvokeCollisionExit2D;
            managedCallbacks->ClassExists = &ScriptInstanceManager.ClassExists;
            managedCallbacks->LoadUserAssembly = &ScriptInstanceManager.LoadUserAssembly;
            managedCallbacks->UnloadUserAssembly = &ScriptInstanceManager.UnloadUserAssembly;
            managedCallbacks->GetScriptFields = &ScriptInstanceManager.GetScriptFields;
            managedCallbacks->SetScriptField = &ScriptInstanceManager.SetScriptField;
            managedCallbacks->GetClassFieldDefs = &ScriptInstanceManager.GetClassFieldDefs;
            managedCallbacks->RaiseApplicationStart = &ScriptInstanceManager.RaiseApplicationStart;
            managedCallbacks->RaiseApplicationPaused = &ScriptInstanceManager.RaiseApplicationPaused;
            managedCallbacks->RaiseApplicationQuit = &ScriptInstanceManager.RaiseApplicationQuit;
            managedCallbacks->RaiseFocusChanged = &ScriptInstanceManager.RaiseFocusChanged;
            managedCallbacks->RaiseKeyDown = &ScriptInstanceManager.RaiseKeyDown;
            managedCallbacks->RaiseKeyUp = &ScriptInstanceManager.RaiseKeyUp;
            managedCallbacks->RaiseMouseDown = &ScriptInstanceManager.RaiseMouseDown;
            managedCallbacks->RaiseMouseUp = &ScriptInstanceManager.RaiseMouseUp;
            managedCallbacks->RaiseMouseScroll = &ScriptInstanceManager.RaiseMouseScroll;
            managedCallbacks->RaiseMouseMove = &ScriptInstanceManager.RaiseMouseMove;
            managedCallbacks->RaiseBeforeSceneLoaded = &ScriptInstanceManager.RaiseBeforeSceneLoaded;
            managedCallbacks->RaiseSceneLoaded = &ScriptInstanceManager.RaiseSceneLoaded;
            managedCallbacks->RaiseBeforeSceneUnloaded = &ScriptInstanceManager.RaiseBeforeSceneUnloaded;
            managedCallbacks->RaiseSceneUnloaded = &ScriptInstanceManager.RaiseSceneUnloaded;
            managedCallbacks->CreateGameSystemInstance = &ScriptInstanceManager.CreateGameSystemInstance;
            managedCallbacks->DestroyGameSystemInstance = &ScriptInstanceManager.DestroyGameSystemInstance;
            managedCallbacks->InvokeGameSystemStart = &ScriptInstanceManager.InvokeGameSystemStart;
            managedCallbacks->InvokeGameSystemUpdate = &ScriptInstanceManager.InvokeGameSystemUpdate;
            managedCallbacks->InvokeGameSystemEnable = &ScriptInstanceManager.InvokeGameSystemEnable;
            managedCallbacks->InvokeGameSystemDisable = &ScriptInstanceManager.InvokeGameSystemDisable;
            managedCallbacks->InvokeGameSystemDestroy = &ScriptInstanceManager.InvokeGameSystemDestroy;
            managedCallbacks->GameSystemClassExists = &ScriptInstanceManager.GameSystemClassExists;
            managedCallbacks->CreateGlobalSystemInstance = &ScriptInstanceManager.CreateGlobalSystemInstance;
            managedCallbacks->DestroyGlobalSystemInstance = &ScriptInstanceManager.DestroyGlobalSystemInstance;
            managedCallbacks->InvokeGlobalSystemInitialize = &ScriptInstanceManager.InvokeGlobalSystemInitialize;
            managedCallbacks->InvokeGlobalSystemUpdate = &ScriptInstanceManager.InvokeGlobalSystemUpdate;
            managedCallbacks->InvokeGlobalSystemEnable = &ScriptInstanceManager.InvokeGlobalSystemEnable;
            managedCallbacks->InvokeGlobalSystemDisable = &ScriptInstanceManager.InvokeGlobalSystemDisable;
            managedCallbacks->GlobalSystemClassExists = &ScriptInstanceManager.GlobalSystemClassExists;

            // ── New lifecycle slots (appended for binary compat) ──
            managedCallbacks->InvokeAwake = &ScriptInstanceManager.InvokeAwake;
            managedCallbacks->InvokeFixedUpdate = &ScriptInstanceManager.InvokeFixedUpdate;
            managedCallbacks->InvokeGameSystemAwake = &ScriptInstanceManager.InvokeGameSystemAwake;
            managedCallbacks->InvokeGameSystemFixedUpdate = &ScriptInstanceManager.InvokeGameSystemFixedUpdate;
            managedCallbacks->InvokeGlobalSystemFixedUpdate = &ScriptInstanceManager.InvokeGlobalSystemFixedUpdate;

            // ── GameSystem field reflection (appended for binary compat) ──
            managedCallbacks->GetGameSystemFields = &ScriptInstanceManager.GetGameSystemFields;
            managedCallbacks->SetGameSystemField = &ScriptInstanceManager.SetGameSystemField;

            // ── UI event dispatch (appended for binary compat) ──
            managedCallbacks->RaiseUiEventDispatch = &ScriptInstanceManager.RaiseUiEventDispatch;

            // ── Coroutine pump (appended for binary compat) ──
            managedCallbacks->PumpCoroutinesUpdate = &ScriptInstanceManager.PumpCoroutinesUpdate;
            managedCallbacks->PumpCoroutinesFixedUpdate = &ScriptInstanceManager.PumpCoroutinesFixedUpdate;

            // ── Inspector event bindings (appended for binary compat) ──
            managedCallbacks->GetInvokableMethodsBuffer = &ScriptInstanceManager.GetInvokableMethodsBuffer;
            managedCallbacks->InvokeScriptMethodByName = &ScriptInstanceManager.InvokeScriptMethodByName;

            ScriptInstanceManager.SetCoreAssembly(typeof(ScriptHostBridge).Assembly);

            // Install the main-thread sync context so any `await` inside
            // a script resumes on the engine main thread via our per-frame
            // pump, not on a thread-pool thread. Must run before any user
            // code (LoadUserAssembly happens later, after this returns).
            SynchronizationContext.SetSynchronizationContext(AxiomSynchronizationContext.Instance);
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"ScriptHostBridge.Initialize failed: {ex}");
            return -1;
        }
    }
}
