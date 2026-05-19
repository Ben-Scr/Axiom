using System;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json;
using Index.Components;
using Index.Interop;

namespace Index;

public enum EntityOrigin
{
    Scene = 0,
    Prefab = 1,
    Runtime = 2
}

public class Entity : IEquatable<Entity>
{
    public static readonly Entity Invalid = new(0);

    // Shared identity cache for managed Component wrappers — keyed by (entity, type) so any
    // Entity wrapper instance pointing at the same entity ID resolves the same Component
    // object. Originally added to preserve managed-script field state across cached lookups;
    // now also load-bearing for *native* UI components, which carry per-instance event
    // subscriptions (Button.OnClicked, Slider.OnValueChanged, etc.). UIEventDispatcher
    // creates fresh Entity wrappers each frame, so without this shared store the dispatcher
    // would see a different Button instance than the user's script and the events would
    // silently no-op.
    private static readonly Dictionary<(ulong EntityID, Type ComponentType), Component> s_ManagedComponentStore = new();

    private readonly Dictionary<Type, Component> m_ComponentCache = new();
    private readonly ulong m_PrefabAssetGUID;
    private Transform2D? m_TransformComponent;

    protected Entity()
    {
        ID = 0;
        m_PrefabAssetGUID = 0;
    }

    internal Entity(ulong id)
    {
        ID = id;
        m_PrefabAssetGUID = 0;
    }

    private Entity(ulong id, ulong prefabAssetGuid)
    {
        ID = id;
        m_PrefabAssetGUID = prefabAssetGuid;
    }

    public readonly ulong ID;

    internal bool IsPrefabAsset => m_PrefabAssetGUID != 0;
    public ulong RuntimeID => IsPrefabAsset ? 0 : InternalCalls.Entity_GetRuntimeID(ID);
    public ulong SceneGUID => IsPrefabAsset ? 0 : InternalCalls.Entity_GetSceneGUID(ID);
    public ulong PrefabGUID => IsPrefabAsset ? m_PrefabAssetGUID : InternalCalls.Entity_GetPrefabGUID(ID);
    public EntityOrigin Origin => IsPrefabAsset ? EntityOrigin.Prefab : (EntityOrigin)InternalCalls.Entity_GetOrigin(ID);
    public bool IsSceneEntity => !IsPrefabAsset && Origin == EntityOrigin.Scene;
    public bool IsPrefabInstance => IsPrefabAsset || (!IsPrefabAsset && Origin == EntityOrigin.Prefab);
    public bool IsRuntime => !IsPrefabAsset && Origin == EntityOrigin.Runtime;
    public bool IsStatic
    {
        get => !IsPrefabAsset && InternalCalls.Entity_GetIsStatic(ID);
        set
        {
            if (!IsPrefabAsset)
                InternalCalls.Entity_SetIsStatic(ID, value);
        }
    }

    // Authored "Enabled" — mirrors the inspector checkbox. True when
    // the user hasn't disabled this specific entity, even if an
    // ancestor is disabled (in which case the engine still treats this
    // as inactive at runtime — see IsEnabledInHierarchy). Setting this
    // toggles the user's intent: when the parent later re-enables, an
    // authored-enabled child comes back; an authored-disabled child
    // stays off.
    public bool IsEnabled
    {
        get => !IsPrefabAsset && InternalCalls.Entity_GetIsEnabled(ID);
        set
        {
            if (!IsPrefabAsset)
                InternalCalls.Entity_SetIsEnabled(ID, value);
        }
    }

    // Effective active flag — true only when this entity AND every
    // ancestor are enabled. Matches what engine systems see (they
    // filter their views with the runtime DisabledTag). Use this for
    // gameplay checks like "is this widget actually live this frame";
    // use IsEnabled to read or write the inspector-visible authored
    // state.
    public bool IsEnabledInHierarchy
        => !IsPrefabAsset && InternalCalls.Entity_GetIsEnabledInHierarchy(ID);

    public Transform2D Transform
    {
        get
        {
            if (m_TransformComponent == null)
            {
                m_TransformComponent = GetComponent<Transform2D>();
                if (m_TransformComponent == null)
                    throw new InvalidOperationException("Entity does not have a Transform2D component");
            }
            return m_TransformComponent;
        }
        set => m_TransformComponent = value;
    }

    public string Name
    {
        get
        {
            if (IsPrefabAsset)
                return InternalCalls.Asset_GetDisplayName(m_PrefabAssetGUID);

            return GetComponent<NameComponent>()?.Name ?? "";
        }
        set
        {
            if (IsPrefabAsset)
                return;

            if (string.IsNullOrEmpty(value))
            {
                RemoveComponent<NameComponent>();
                return;
            }

            NameComponent? nameComp = GetComponent<NameComponent>() ?? AddComponent<NameComponent>();
            if (nameComp != null)
                nameComp.Name = value;
        }
    }

    private static readonly Dictionary<Type, string> s_NativeComponentNames = new()
    {
        { typeof(NameComponent),         "Name" },
        { typeof(Transform2D),           "Transform 2D" },
        { typeof(SpriteRenderer),        "Sprite Renderer" },
        { typeof(TextRenderer),          "Text Renderer" },
        { typeof(Camera2D),              "Camera 2D" },
        { typeof(Rigidbody2D),           "Rigidbody 2D" },
        { typeof(BoxCollider2D),         "Box Collider 2D" },
        { typeof(CircleCollider2D),      "Circle Collider 2D" },
        { typeof(PolygonCollider2D),     "Polygon Collider 2D" },
        { typeof(AudioSource),           "Audio Source" },
        { typeof(FastBody2D),            "Fast Body 2D" },
        { typeof(FastBoxCollider2D),     "Fast Box Collider 2D" },
        { typeof(FastCircleCollider2D),  "Fast Circle Collider 2D" },
        { typeof(ParticleSystem2D),      "Particle System 2D" },

        // ── UI (Index.UI namespace) ──────────────────────────────────
        // Map each C# UI wrapper to the native ComponentInfo display
        // name registered in BuiltInComponentRegistration.cpp so
        // AddComponent / HasComponent / RemoveComponent reach the right
        // C++ type. Class names alone don't carry the "2D" suffix that
        // RectTransform2DComponent uses on the C++ side.
        { typeof(Index.UI.RectTransform),  "Rect Transform 2D" },
        { typeof(Index.UI.Image),          "Image" },
        { typeof(Index.UI.Interactable),   "Interactable" },
        { typeof(Index.UI.Button),         "Button" },
        { typeof(Index.UI.Slider),         "Slider" },
        { typeof(Index.UI.Toggle),         "Toggle" },
        { typeof(Index.UI.InputField),     "Input Field" },
        { typeof(Index.UI.Dropdown),       "Dropdown" },

        // ── UI (additional) ─────────────────────────────────────────
        // Strings must match the native display name registered in
        // BuiltInComponentRegistration.cpp — Entity_HasComponent /
        // Scene_QueryEntities look up native components by this name.
        { typeof(Index.UI.ScrollRect),            "Scroll Rect" },
        { typeof(Index.UI.Scrollbar),             "Scrollbar" },
        { typeof(Index.UI.Mask),                  "Mask" },
        { typeof(Index.UI.CircularSlider),        "Circular Slider" },
        { typeof(Index.UI.HorizontalLayoutGroup), "Horizontal Layout Group" },
        { typeof(Index.UI.VerticalLayoutGroup),   "Vertical Layout Group" },
        { typeof(Index.UI.GridLayoutGroup),       "Grid Layout Group" },
        { typeof(Index.UI.ContentSizeFitter),     "Content Size Fitter" },
        { typeof(Index.UI.WidthConstraint),       "Width Constraint" },
    };

    private static string? GetNativeName<T>() where T : Component, new() => GetComponentName(typeof(T));

    private static string? GetComponentName(Type type)
    {
        if (s_NativeComponentNames.TryGetValue(type, out string? name))
            return name;

        return type.IsSubclassOf(typeof(Component)) ? type.Name : null;
    }

    private static Type? ResolveComponentTypeByName(string name)
    {
        if (string.IsNullOrWhiteSpace(name))
            return null;

        foreach (var pair in s_NativeComponentNames)
        {
            Type type = pair.Key;
            if (string.Equals(pair.Value, name, StringComparison.Ordinal)
                || string.Equals(type.Name, name, StringComparison.Ordinal)
                || string.Equals(type.FullName, name, StringComparison.Ordinal))
            {
                return type;
            }
        }

        foreach (Assembly assembly in AppDomain.CurrentDomain.GetAssemblies())
        {
            Type[] types;
            try { types = assembly.GetTypes(); }
            catch (ReflectionTypeLoadException ex)
            {
                List<Type> loadedTypes = new();
                foreach (Type? type in ex.Types)
                {
                    if (type != null)
                        loadedTypes.Add(type);
                }
                types = loadedTypes.ToArray();
            }

            foreach (Type type in types)
            {
                if (!type.IsSubclassOf(typeof(Component)))
                    continue;

                if (string.Equals(type.Name, name, StringComparison.Ordinal)
                    || string.Equals(type.FullName, name, StringComparison.Ordinal))
                {
                    return type;
                }
            }
        }

        return null;
    }

    internal static Entity FromPrefabGUID(ulong prefabGuid)
        => prefabGuid != 0 ? new Entity(0, prefabGuid) : Invalid;

    internal static string? GetNativeComponentName<T>() where T : Component, new() => GetNativeName<T>();
    internal static bool TryGetNativeComponentName(Type type, out string? name)
    {
        if (s_NativeComponentNames.TryGetValue(type, out name))
            return true;

        if (type.IsValueType && typeof(IComponent).IsAssignableFrom(type))
        {
            name = ComponentTypes.TryGetNativeName(type);
            return name != null;
        }

        name = null;
        return false;
    }

    private void InvalidateCachedComponent(Type type)
    {
        if (m_ComponentCache.Remove(type, out Component? cached))
            cached.Invalidate();

        if (type == typeof(Transform2D))
            m_TransformComponent = null;

        InvalidateManagedComponent(ID, type);
    }

    private void InvalidateAllCachedComponents()
    {
        foreach (Component component in m_ComponentCache.Values)
            component.Invalidate();

        m_ComponentCache.Clear();
        m_TransformComponent = null;
        InvalidateManagedComponents(ID);
    }

    private static void InvalidateManagedComponent(ulong entityID, Type type)
    {
        if (s_ManagedComponentStore.Remove((entityID, type), out Component? component))
            component.Invalidate();
    }

    private static void InvalidateManagedComponents(ulong entityID)
    {
        List<(ulong EntityID, Type ComponentType)> keysToRemove = new();
        foreach (var key in s_ManagedComponentStore.Keys)
        {
            if (key.EntityID == entityID)
                keysToRemove.Add(key);
        }

        foreach (var key in keysToRemove)
        {
            if (s_ManagedComponentStore.Remove(key, out Component? component))
                component.Invalidate();
        }
    }

    // Hot-reload support: the AssemblyLoadContext that owns user-defined Component
    // subclasses can only unload once *every* instance is unreachable. Component
    // entries in s_ManagedComponentStore reference user types and therefore root
    // the context — clearing this dictionary on assembly unload is required for
    // the ALC to actually collect and free its types.
    internal static void ClearManagedComponentStore()
    {
        foreach (var component in s_ManagedComponentStore.Values)
            component.Invalidate();
        s_ManagedComponentStore.Clear();
    }

    public T? AddComponent<T>() where T : Component, new()
    {
        if (IsPrefabAsset)
            return null;

        // Script branch: T : EntityScript routes through the script
        // backend (ScriptComponent.Scripts + ScriptInstanceManager).
        // C# can't disambiguate by constraint (CS0111), so we dispatch
        // at runtime on typeof(T). The script's OnAwake / OnStart fire
        // immediately via the native BindAndAwakeNow path so the
        // returned instance is fully live before this method returns.
        if (typeof(EntityScript).IsAssignableFrom(typeof(T)))
        {
            if (!InternalCalls.Entity_AddScript(ID, typeof(T).Name))
                return null;
            return ScriptInstanceManager.GetScriptInstance(ID, typeof(T).Name) as T;
        }

        string? nativeName = GetNativeName<T>();
        if (nativeName == null)
            return null;

        if (!InternalCalls.Entity_AddComponent(ID, nativeName))
            return null;

        return GetComponent<T>();
    }

    // Native (`IComponent`) variant — adds a blittable native-backed component
    // to the entity's C++ ECS pool. Mirrors the managed-`Component`
    // `AddComponent<T>()` above but uses `ComponentTypes<T>.NativeName`
    // (resolved once per T via the static-generic cache) instead of the
    // s_NativeComponentNames map. Returns true if the component was newly
    // added or already present after the call; false if the entity is a
    // prefab asset or the native pool refused the insertion. Pair with
    // `GetRef<T>()` to read/write the data.
    //
    // Distinct method name (not an overload of `AddComponent<T>`) because
    // C# overload resolution ignores generic constraints — two
    // `AddComponent<T>()` methods that differ only by `where T : Component`
    // vs. `where T : unmanaged, IComponent` produce CS0111. For bulk
    // creation of many entities use `EntityCommandBuffer` instead, which
    // skips the per-call P/Invoke entirely.
    public bool AddNativeComponent<T>() where T : unmanaged, IComponent
    {
        if (IsPrefabAsset)
            return false;

        return InternalCalls.Entity_AddComponent(ID, ComponentTypes<T>.NativeName);
    }

    public bool HasComponent<T>() where T : Component, new()
    {
        if (IsPrefabAsset)
            return false;

        // Script branch (see AddComponent for the CS0111 rationale).
        if (typeof(EntityScript).IsAssignableFrom(typeof(T)))
            return InternalCalls.Entity_HasScript(ID, typeof(T).Name);

        string? nativeName = GetNativeName<T>();
        return nativeName != null && InternalCalls.Entity_HasComponent(ID, nativeName);
    }

    /// Native (`IComponent`) variant — see <see cref="AddNativeComponent{T}"/>
    /// for the naming rationale.
    public bool HasNativeComponent<T>() where T : unmanaged, IComponent
    {
        if (IsPrefabAsset)
            return false;

        return InternalCalls.Entity_HasComponent(ID, ComponentTypes<T>.NativeName);
    }

    public bool RemoveComponent<T>() where T : Component, new()
    {
        if (IsPrefabAsset)
            return false;

        // Script branch (see AddComponent for the CS0111 rationale).
        if (typeof(EntityScript).IsAssignableFrom(typeof(T)))
            return InternalCalls.Entity_RemoveScript(ID, typeof(T).Name);

        string? nativeName = GetNativeName<T>();
        if (nativeName == null) return false;

        bool removed = InternalCalls.Entity_RemoveComponent(ID, nativeName);
        if (removed || !HasComponent<T>())
            InvalidateCachedComponent(typeof(T));

        return removed;
    }

    // Native (`IComponent`) variant — removes a blittable native-backed
    // component from the entity's C++ ECS pool. No managed cache invalidation
    // is needed because IComponent structs aren't cached via
    // s_ManagedComponentStore / m_ComponentCache (those store Component class
    // wrappers, not unmanaged structs). Renamed away from `RemoveComponent`
    // for the same CS0111 reason as the Add/Has pair above.
    public bool RemoveNativeComponent<T>() where T : unmanaged, IComponent
    {
        if (IsPrefabAsset)
            return false;

        return InternalCalls.Entity_RemoveComponent(ID, ComponentTypes<T>.NativeName);
    }

    public T? GetComponent<T>() where T : Component, new()
    {
        return GetComponentByType(typeof(T)) as T;
    }

    /// <summary>
    /// Out-param variant of <see cref="GetComponent{T}"/>. Works for any
    /// T : Component, new() — including EntityScript subclasses, which
    /// resolve via the script storage (ScriptInstanceManager). Returns
    /// true when the component / script is present on the entity.
    /// </summary>
    public bool TryGetComponent<T>(out T? value) where T : Component, new()
    {
        value = GetComponent<T>();
        return value != null;
    }

    // ── HasComponents<T1..T8> (variadic AND-semantics) ───────────────
    //
    // Per-T constraint is `Component, new()` so the same arities cover
    // managed components AND scripts (EntityScript : Component). Mixing
    // shapes in one call is fine — each HasComponent<Ti>() dispatches
    // on typeof(Ti) at runtime. For native IComponent structs, use the
    // separately-named HasNativeComponents counterpart (added below)
    // since C# can't unify `Component`-constrained generics with
    // `unmanaged, IComponent`-constrained generics (CS0111).

    public bool HasComponents<T1>()
        where T1 : Component, new()
        => HasComponent<T1>();

    public bool HasComponents<T1, T2>()
        where T1 : Component, new()
        where T2 : Component, new()
        => HasComponent<T1>() && HasComponent<T2>();

    public bool HasComponents<T1, T2, T3>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        => HasComponent<T1>() && HasComponent<T2>() && HasComponent<T3>();

    public bool HasComponents<T1, T2, T3, T4>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        => HasComponent<T1>() && HasComponent<T2>() && HasComponent<T3>()
           && HasComponent<T4>();

    public bool HasComponents<T1, T2, T3, T4, T5>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        => HasComponent<T1>() && HasComponent<T2>() && HasComponent<T3>()
           && HasComponent<T4>() && HasComponent<T5>();

    public bool HasComponents<T1, T2, T3, T4, T5, T6>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        where T6 : Component, new()
        => HasComponent<T1>() && HasComponent<T2>() && HasComponent<T3>()
           && HasComponent<T4>() && HasComponent<T5>() && HasComponent<T6>();

    public bool HasComponents<T1, T2, T3, T4, T5, T6, T7>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        where T6 : Component, new()
        where T7 : Component, new()
        => HasComponent<T1>() && HasComponent<T2>() && HasComponent<T3>()
           && HasComponent<T4>() && HasComponent<T5>() && HasComponent<T6>()
           && HasComponent<T7>();

    public bool HasComponents<T1, T2, T3, T4, T5, T6, T7, T8>()
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        where T6 : Component, new()
        where T7 : Component, new()
        where T8 : Component, new()
        => HasComponent<T1>() && HasComponent<T2>() && HasComponent<T3>()
           && HasComponent<T4>() && HasComponent<T5>() && HasComponent<T6>()
           && HasComponent<T7>() && HasComponent<T8>();

    public object? GetComponent(string componentOrScriptName)
    {
        Type? componentType = ResolveComponentTypeByName(componentOrScriptName);
        if (componentType != null)
            return GetComponentByType(componentType);

        return GetScript(componentOrScriptName);
    }

    public EntityScript? GetScript(string scriptName)
    {
        if (IsPrefabAsset)
            return null;

        return ScriptInstanceManager.GetScriptInstance(ID, scriptName);
    }

    // Direct ref into the entity's component storage. Returns a ref-to-null
    // (Unsafe.IsNullRef) when the component or entity is missing — callers in
    // hot paths can branch on that without an exception. For the common case
    // where the script's contract already guarantees the component (a script
    // that requires Transform2D on its host entity), use GetRefOrThrow<T>
    // instead to skip the IsNullRef check.
    //
    // The returned ref is valid only until the next structural change to the
    // same component pool (Add/Remove/Destroy on any T). Refetch each frame.
    public unsafe ref T GetRef<T>() where T : unmanaged, IComponent
    {
        void* p = InternalCalls.Entity_GetComponentPtr(ID, ComponentTypes<T>.NativeName);
        if (p == null) return ref Unsafe.NullRef<T>();
        return ref Unsafe.AsRef<T>(p);
    }

    // Convenience: throws if the component is absent. Most scripts have a
    // hard requirement on their host entity's components and shouldn't pay
    // the IsNullRef branch on every access. The throw collapses to a single
    // null-check in the JIT'd code; the message names the type for debugging.
    public unsafe ref T GetRefOrThrow<T>() where T : unmanaged, IComponent
    {
        void* p = InternalCalls.Entity_GetComponentPtr(ID, ComponentTypes<T>.NativeName);
        if (p == null)
            throw new InvalidOperationException(
                $"Entity {ID} has no '{ComponentTypes<T>.NativeName}' component.");
        return ref Unsafe.AsRef<T>(p);
    }

    // Shortcut for the most-common ref: `entity.TransformRef.LocalPosition += v`.
    // Chains exactly like today's `entity.Transform.Position` — ref-returns are
    // lvalues, so `+= v` writes through to EnTT storage. Skips the per-frame
    // GetComponent<Transform2D>() class-wrapper alloc + dictionary lookup.
    public unsafe ref Native.NativeTransform2D TransformRef
    {
        get
        {
            void* p = InternalCalls.Entity_GetComponentPtr(ID, Native.NativeTransform2D.NativeName);
            if (p == null)
                throw new InvalidOperationException(
                    $"Entity {ID} has no Transform 2D component.");
            return ref Unsafe.AsRef<Native.NativeTransform2D>(p);
        }
    }

    // Non-generic resolution path used by GetComponent<T> AND by script-field
    // deserialization (ParseFieldValue) so an inspector-assigned `Button` /
    // `Slider` / etc. field returns the SAME wrapper that subsequent
    // entity.GetComponent<T>() calls return. Without this, a script field
    // and the UIEventDispatcher would each see a different Component
    // instance — the user's `+= handler` would attach to one and the
    // dispatcher would invoke the other, silently dropping every event
    // (the original "OnClicked never fires" bug). The cache is keyed on
    // (entityID, type) so ANY Entity wrapper pointing at the same id
    // collapses to one shared Component instance.
    internal Component? GetComponentByType(Type type)
    {
        if (IsPrefabAsset)
            return null;

        // Script branch: scripts live in ScriptInstanceManager keyed by
        // (entityID, className). EntityScript : Component, so the same
        // `T : Component, new()` constrained call site resolves into
        // either backend based on runtime type. Returning the script
        // instance directly bypasses m_ComponentCache (which is for
        // Component class-wrappers around ECS data, not script
        // instances that already cache their own state).
        if (typeof(EntityScript).IsAssignableFrom(type))
            return ScriptInstanceManager.GetScriptInstance(ID, type.Name);

        string? nativeName = GetComponentName(type);
        if (nativeName == null)
            return null;

        if (!InternalCalls.Entity_HasComponent(ID, nativeName))
        {
            InvalidateCachedComponent(type);
            return null;
        }

        if (m_ComponentCache.TryGetValue(type, out Component? cached))
            return cached;

        if (s_ManagedComponentStore.TryGetValue((ID, type), out Component? storedComponent))
        {
            m_ComponentCache[type] = storedComponent;
            return storedComponent;
        }

        var component = (Component)Activator.CreateInstance(type)!;
        component.Entity = this;
        if (!s_NativeComponentNames.ContainsKey(type))
            ApplyManagedFieldValues(component, InternalCalls.Entity_GetManagedComponentFields(ID, type.Name));

        s_ManagedComponentStore[(ID, type)] = component;
        m_ComponentCache[type] = component;
        return component;
    }

    private static void ApplyManagedFieldValues(Component component, string json)
    {
        if (string.IsNullOrWhiteSpace(json) || json == "{}")
            return;

        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            if (doc.RootElement.ValueKind != JsonValueKind.Object)
                return;

            const BindingFlags k_Flags = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance;
            Type type = component.GetType();
            foreach (JsonProperty property in doc.RootElement.EnumerateObject())
            {
                string raw = property.Value.GetString() ?? "";

                // Field path first (matches the historical behaviour and
                // is the cheap path).
                FieldInfo? field = type.GetField(property.Name, k_Flags);
                if (field != null)
                {
                    object? value = ParseManagedFieldValue(field.FieldType, raw);
                    if (value != null)
                        field.SetValue(component, value);
                    continue;
                }

                // Fall back to a property if no field with that name —
                // lets a managed component expose validated setters via
                // public properties and still round-trip through the
                // scene file. Honour the same setter-must-be-public
                // contract the inspector applies; properties without an
                // accessible setter can't accept the deserialized value.
                PropertyInfo? prop = type.GetProperty(property.Name, k_Flags);
                if (prop == null) continue;
                if (!prop.CanWrite || prop.SetMethod == null || !prop.SetMethod.IsPublic) continue;

                object? propValue = ParseManagedFieldValue(prop.PropertyType, raw);
                if (propValue == null) continue;

                try
                {
                    prop.SetValue(component, propValue);
                }
                catch
                {
                    // Validation in the user's setter rejected the
                    // stored value (e.g. saved scene predates a tighter
                    // invariant). Outer try-catch already swallows, but
                    // keep this local so one bad property doesn't abort
                    // the rest of the field bag.
                }
            }
        }
        catch
        {
        }
    }

    private static object? ParseManagedFieldValue(Type type, string value)
    {
        CultureInfo ic = CultureInfo.InvariantCulture;

        if (type == typeof(string)) return value;
        if (type == typeof(bool)) return value == "true" || value == "True" || value == "1";
        if (type == typeof(float) && float.TryParse(value, NumberStyles.Float, ic, out float f)) return f;
        if (type == typeof(double) && double.TryParse(value, NumberStyles.Float, ic, out double d)) return d;
        if (type == typeof(int) && int.TryParse(value, NumberStyles.Integer, ic, out int i)) return i;
        if (type == typeof(uint) && uint.TryParse(value, NumberStyles.Integer, ic, out uint ui)) return ui;
        if (type == typeof(long) && long.TryParse(value, NumberStyles.Integer, ic, out long l)) return l;
        if (type == typeof(ulong) && ulong.TryParse(value, NumberStyles.Integer, ic, out ulong ul)) return ul;
        if (type == typeof(short) && short.TryParse(value, NumberStyles.Integer, ic, out short s)) return s;
        if (type == typeof(ushort) && ushort.TryParse(value, NumberStyles.Integer, ic, out ushort us)) return us;
        if (type == typeof(byte) && byte.TryParse(value, NumberStyles.Integer, ic, out byte b)) return b;
        if (type == typeof(sbyte) && sbyte.TryParse(value, NumberStyles.Integer, ic, out sbyte sb)) return sb;
        if (type == typeof(Entity)) return ParseEntityReference(value);
        if (type == typeof(Scene)) return Scene.FromAssetUUID(ParseAssetUUID(value));
        if (type == typeof(Texture)) return Texture.FromAssetUUID(ParseAssetUUID(value));
        if (type == typeof(Audio)) return Audio.FromAssetUUID(ParseAssetUUID(value));
        if (type == typeof(TextureRef)) return new TextureRef(ParseAssetUUID(value));
        if (type == typeof(AudioRef)) return new AudioRef(ParseAssetUUID(value));

        string[] parts = value.Split(',', StringSplitOptions.TrimEntries);
        if (type == typeof(Vector2) && parts.Length >= 2)
            return new Vector2(float.Parse(parts[0], ic), float.Parse(parts[1], ic));
        if (type == typeof(Vector2Int) && parts.Length >= 2)
            return new Vector2Int(int.Parse(parts[0], ic), int.Parse(parts[1], ic));
        if (type == typeof(Vector3) && parts.Length >= 3)
            return new Vector3(float.Parse(parts[0], ic), float.Parse(parts[1], ic), float.Parse(parts[2], ic));
        if (type == typeof(Vector3Int) && parts.Length >= 3)
            return new Vector3Int(int.Parse(parts[0], ic), int.Parse(parts[1], ic), int.Parse(parts[2], ic));
        if (type == typeof(Vector4) && parts.Length >= 4)
            return new Vector4(float.Parse(parts[0], ic), float.Parse(parts[1], ic), float.Parse(parts[2], ic), float.Parse(parts[3], ic));
        if (type == typeof(Vector4Int) && parts.Length >= 4)
            return new Vector4Int(int.Parse(parts[0], ic), int.Parse(parts[1], ic), int.Parse(parts[2], ic), int.Parse(parts[3], ic));
        if (type == typeof(Color) && parts.Length >= 4)
            return new Color(float.Parse(parts[0], ic), float.Parse(parts[1], ic), float.Parse(parts[2], ic), float.Parse(parts[3], ic));

        return null;
    }

    internal static ulong ParseAssetUUID(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return 0;

        if (ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out ulong assetId))
            return assetId;

        return InternalCalls.Asset_GetOrCreateUUIDFromPath(value);
    }

    internal static Entity ParseEntityReference(string value)
    {
        if (string.IsNullOrWhiteSpace(value) || value == "0")
            return Invalid;

        if (value.StartsWith("prefab:", StringComparison.OrdinalIgnoreCase)
            && ulong.TryParse(value.AsSpan("prefab:".Length), NumberStyles.None, CultureInfo.InvariantCulture, out ulong prefabGuid))
        {
            return FromPrefabGUID(prefabGuid);
        }

        return ulong.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out ulong id) && id != 0
            ? new Entity(id)
            : Invalid;
    }

    public static Entity? FindByName(string name)
    {
        ulong id = InternalCalls.Entity_FindByName(name);
        return id != 0 ? new Entity(id) : null;
    }

    public static Entity Create(string? name = null)
    {
        ulong id = InternalCalls.Entity_Create(name);
        return id != 0 ? new Entity(id) : Invalid;
    }

    public static Entity Instantiate(Entity source)
    {
        if (source is null || source == Invalid)
            return Invalid;

        ulong id = source.IsPrefabAsset
            ? InternalCalls.Entity_InstantiatePrefab(source.PrefabGUID)
            : InternalCalls.Entity_Clone(source.ID);

        return id != 0 ? new Entity(id) : Invalid;
    }

    public static Entity Create(Entity source) => Instantiate(source);

    // ── CreateWith / CreateWithNative ────────────────────────────────
    //
    // Convenience overloads for "create an entity, then attach K components
    // in one expression". The body is a manual Create + AddComponent loop —
    // each component is still a separate P/Invoke. For high-volume spawning
    // with many components per entity, use EntityCommandBuffer instead; it
    // amortizes the P/Invoke and component-resolution cost over the whole
    // batch (~50–100× faster on the dominant cases).
    //
    // Two parallel families:
    //   - `CreateWith<T...>`        — managed Component subclasses
    //     (`where T : Component, new()`)
    //   - `CreateWithNative<T...>`  — native IComponent structs
    //     (`where T : unmanaged, IComponent`)
    //
    // Distinct method names (not overloads on the constraint) because C#
    // overload resolution ignores generic constraints (CS0111) — same
    // reason `AddComponent` vs. `AddNativeComponent` are distinct names.
    // Returns `Invalid` if the entity could not be created; component
    // attach failures (e.g. unregistered managed type) are silently
    // ignored to match the per-component AddComponent behaviour.

    public static Entity CreateWith<T1>(string? name = null)
        where T1 : Component, new()
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddComponent<T1>();
        return e;
    }

    public static Entity CreateWith<T1, T2>(string? name = null)
        where T1 : Component, new()
        where T2 : Component, new()
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddComponent<T1>();
        _ = e.AddComponent<T2>();
        return e;
    }

    public static Entity CreateWith<T1, T2, T3>(string? name = null)
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddComponent<T1>();
        _ = e.AddComponent<T2>();
        _ = e.AddComponent<T3>();
        return e;
    }

    public static Entity CreateWith<T1, T2, T3, T4>(string? name = null)
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddComponent<T1>();
        _ = e.AddComponent<T2>();
        _ = e.AddComponent<T3>();
        _ = e.AddComponent<T4>();
        return e;
    }

    public static Entity CreateWith<T1, T2, T3, T4, T5>(string? name = null)
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddComponent<T1>();
        _ = e.AddComponent<T2>();
        _ = e.AddComponent<T3>();
        _ = e.AddComponent<T4>();
        _ = e.AddComponent<T5>();
        return e;
    }

    public static Entity CreateWith<T1, T2, T3, T4, T5, T6>(string? name = null)
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        where T6 : Component, new()
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddComponent<T1>();
        _ = e.AddComponent<T2>();
        _ = e.AddComponent<T3>();
        _ = e.AddComponent<T4>();
        _ = e.AddComponent<T5>();
        _ = e.AddComponent<T6>();
        return e;
    }

    public static Entity CreateWith<T1, T2, T3, T4, T5, T6, T7>(string? name = null)
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        where T6 : Component, new()
        where T7 : Component, new()
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddComponent<T1>();
        _ = e.AddComponent<T2>();
        _ = e.AddComponent<T3>();
        _ = e.AddComponent<T4>();
        _ = e.AddComponent<T5>();
        _ = e.AddComponent<T6>();
        _ = e.AddComponent<T7>();
        return e;
    }

    public static Entity CreateWith<T1, T2, T3, T4, T5, T6, T7, T8>(string? name = null)
        where T1 : Component, new()
        where T2 : Component, new()
        where T3 : Component, new()
        where T4 : Component, new()
        where T5 : Component, new()
        where T6 : Component, new()
        where T7 : Component, new()
        where T8 : Component, new()
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddComponent<T1>();
        _ = e.AddComponent<T2>();
        _ = e.AddComponent<T3>();
        _ = e.AddComponent<T4>();
        _ = e.AddComponent<T5>();
        _ = e.AddComponent<T6>();
        _ = e.AddComponent<T7>();
        _ = e.AddComponent<T8>();
        return e;
    }

    public static Entity CreateWithNative<T1>(string? name = null)
        where T1 : unmanaged, IComponent
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddNativeComponent<T1>();
        return e;
    }

    public static Entity CreateWithNative<T1, T2>(string? name = null)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddNativeComponent<T1>();
        _ = e.AddNativeComponent<T2>();
        return e;
    }

    public static Entity CreateWithNative<T1, T2, T3>(string? name = null)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddNativeComponent<T1>();
        _ = e.AddNativeComponent<T2>();
        _ = e.AddNativeComponent<T3>();
        return e;
    }

    public static Entity CreateWithNative<T1, T2, T3, T4>(string? name = null)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddNativeComponent<T1>();
        _ = e.AddNativeComponent<T2>();
        _ = e.AddNativeComponent<T3>();
        _ = e.AddNativeComponent<T4>();
        return e;
    }

    public static Entity CreateWithNative<T1, T2, T3, T4, T5>(string? name = null)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddNativeComponent<T1>();
        _ = e.AddNativeComponent<T2>();
        _ = e.AddNativeComponent<T3>();
        _ = e.AddNativeComponent<T4>();
        _ = e.AddNativeComponent<T5>();
        return e;
    }

    public static Entity CreateWithNative<T1, T2, T3, T4, T5, T6>(string? name = null)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddNativeComponent<T1>();
        _ = e.AddNativeComponent<T2>();
        _ = e.AddNativeComponent<T3>();
        _ = e.AddNativeComponent<T4>();
        _ = e.AddNativeComponent<T5>();
        _ = e.AddNativeComponent<T6>();
        return e;
    }

    public static Entity CreateWithNative<T1, T2, T3, T4, T5, T6, T7>(string? name = null)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddNativeComponent<T1>();
        _ = e.AddNativeComponent<T2>();
        _ = e.AddNativeComponent<T3>();
        _ = e.AddNativeComponent<T4>();
        _ = e.AddNativeComponent<T5>();
        _ = e.AddNativeComponent<T6>();
        _ = e.AddNativeComponent<T7>();
        return e;
    }

    public static Entity CreateWithNative<T1, T2, T3, T4, T5, T6, T7, T8>(string? name = null)
        where T1 : unmanaged, IComponent
        where T2 : unmanaged, IComponent
        where T3 : unmanaged, IComponent
        where T4 : unmanaged, IComponent
        where T5 : unmanaged, IComponent
        where T6 : unmanaged, IComponent
        where T7 : unmanaged, IComponent
        where T8 : unmanaged, IComponent
    {
        Entity e = Create(name);
        if (e == Invalid) return Invalid;
        _ = e.AddNativeComponent<T1>();
        _ = e.AddNativeComponent<T2>();
        _ = e.AddNativeComponent<T3>();
        _ = e.AddNativeComponent<T4>();
        _ = e.AddNativeComponent<T5>();
        _ = e.AddNativeComponent<T6>();
        _ = e.AddNativeComponent<T7>();
        _ = e.AddNativeComponent<T8>();
        return e;
    }

    public static void Destroy(Entity entity)
    {
        if (entity is null || entity == Invalid)
            return;
        entity.Destroy();
    }

    /// <summary>
    /// Schedule the destruction <paramref name="delay"/> seconds in the
    /// future. The entity stays valid until the next Scene update on
    /// which (accumulated dt) ≥ delay. Non-positive delay collapses to
    /// immediate destruction. Cached components are invalidated up
    /// front so any stored references stop dereferencing into stale
    /// EnTT slots before the queue drains.
    /// </summary>
    public static void Destroy(Entity entity, float delay)
    {
        if (entity is null || entity == Invalid)
            return;
        entity.Destroy(delay);
    }

    public void Destroy()
    {
        if (IsPrefabAsset)
            return;

        InvalidateAllCachedComponents();
        InternalCalls.Entity_Destroy(ID);
    }

    public void Destroy(float delay)
    {
        if (IsPrefabAsset)
            return;

        if (delay <= 0.0f)
        {
            Destroy();
            return;
        }

        // Don't invalidate cached components here — the entity is still
        // live until the timer fires, and a script that scheduled its
        // own delayed destroy may want to keep accessing its components
        // for the remainder of the delay window. The cache is dropped
        // by the eventual immediate-destroy path on the native side
        // when the queue drains.
        InternalCalls.Entity_DestroyDelayed(ID, delay);
    }

    public bool Equals(Entity? other)
        => other is not null && ID == other.ID && m_PrefabAssetGUID == other.m_PrefabAssetGUID;

    public override bool Equals(object? obj) => obj is Entity other && Equals(other);
    public override int GetHashCode() => HashCode.Combine(ID, m_PrefabAssetGUID);

    public static bool operator ==(Entity? a, Entity? b)
    {
        if (a is null) return b is null;
        return a.Equals(b);
    }

    public static bool operator !=(Entity? a, Entity? b) => !(a == b);

    public static implicit operator bool(Entity? entity)
    {
        if (entity is null || entity == Invalid)
            return false;

        return entity.IsPrefabAsset
            ? InternalCalls.Asset_IsValid(entity.m_PrefabAssetGUID)
            : InternalCalls.Entity_IsValid(entity.ID);
    }
}
