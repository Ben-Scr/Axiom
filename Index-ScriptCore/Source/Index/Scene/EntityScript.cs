using System;
using System.Threading;
using System.Threading.Tasks;
using Index.Coroutines;

namespace Index;

/// <summary>
/// Base class for user C# scripts. Subclass and override lifecycle callbacks.
///
/// Inherits <see cref="Component"/> so the unified Entity API treats scripts
/// like components: <c>entity.GetComponent&lt;MyScript&gt;()</c>,
/// <c>entity.AddComponent&lt;MyScript&gt;()</c>,
/// <c>entity.HasComponent&lt;MyScript&gt;()</c>,
/// <c>entity.TryGetComponent&lt;MyScript&gt;(out var s)</c> all dispatch to
/// the script storage (ScriptInstanceManager) when <c>T : EntityScript</c>,
/// and to the ECS storage otherwise. C# overload resolution does NOT
/// pick between generic constraints (CS0111), so unifying the hierarchy
/// is the only path to a single <c>AddComponent&lt;T&gt;</c> that handles
/// both — runtime dispatch on <c>typeof(T)</c> picks the right backend.
///
/// The inherited <c>Entity</c> property (defined on Component) is reused
/// directly — _SetEntityID still works because Component's setter is
/// internal.
/// </summary>
public abstract class EntityScript : Component
{
    public Transform2D Transform => Entity.Transform;

    // ── Coroutine destroy lifetime ───────────────────────────────
    // Lazy-initialised so scripts that never use coroutines pay zero cost.
    // Once cancelled, m_CoroutineCtsTerminated stays true so subsequent
    // DestroyToken accesses always return a pre-cancelled token — awaits
    // started after destroy short-circuit through GetResult immediately.
    private CancellationTokenSource? m_CoroutineCts;
    private bool m_CoroutineCtsTerminated;

    internal void _SetEntityID(ulong id)
    {
        Entity = new Entity(id);
    }

    protected Entity? FindEntityByName(string name) => Entity.FindByName(name);

    protected T? GetComponent<T>() where T : Component, new() => Entity.GetComponent<T>();
    protected object? GetComponent(string componentOrScriptName) => Entity.GetComponent(componentOrScriptName);
    protected EntityScript? GetScript(string scriptName) => Entity.GetScript(scriptName);
    protected T? AddComponent<T>() where T : Component, new() => Entity.AddComponent<T>();
    protected bool RemoveComponent<T>() where T : Component, new() => Entity.RemoveComponent<T>();

    protected Entity Create(string? name = null) => Entity.Create(name);
    protected Entity Create(Entity source) => Entity.Create(source);
    protected Entity Instantiate(Entity prefabOrSource) => Entity.Instantiate(prefabOrSource);

    /// <summary>
    /// Cancellation token tied to this script's lifetime. Cancelled
    /// immediately before OnDestroy() runs. Pass it to any external async
    /// work you start so the work tears itself down with the script.
    /// </summary>
    protected CancellationToken DestroyToken
    {
        get
        {
            if (m_CoroutineCtsTerminated)
                return new CancellationToken(canceled: true);
            return (m_CoroutineCts ??= new CancellationTokenSource()).Token;
        }
    }

    /// <summary>
    /// Recommended fire-and-forget entry point for `async Task` coroutines.
    /// Wraps the task so OperationCanceledException is silently swallowed
    /// (the expected outcome on script destroy) and other exceptions are
    /// logged instead of escaping to TaskScheduler.UnobservedTaskException.
    /// </summary>
    /// <example>
    /// <code>
    /// public override void OnStart() => RunCoroutine(BombSequence);
    /// async Task BombSequence() {
    ///     await new WaitForSeconds(3.0f);
    ///     Entity.Destroy();
    /// }
    /// </code>
    /// </example>
    protected void RunCoroutine(Func<Task> coroutine)
    {
        CancellationToken token = DestroyToken;
        CancellationToken previous = CoroutineContext.CurrentToken.Value;
        CoroutineContext.CurrentToken.Value = token;
        try
        {
            _ = ObserveCoroutine(coroutine());
        }
        finally
        {
            CoroutineContext.CurrentToken.Value = previous;
        }
    }

    private static async Task ObserveCoroutine(Task task)
    {
        try { await task; }
        catch (OperationCanceledException) { }
        catch (Exception ex) { Log.Error($"[Coroutine] {ex}"); }
    }

    /// <summary>
    /// Cancel any pending coroutines owned by this script. Called by
    /// ScriptInstanceManager.InvokeOnDestroy immediately before the user's
    /// OnDestroy override runs, and as belt-and-braces from
    /// UnloadUserAssembly on hot reload.
    /// </summary>
    internal void _CancelPendingCoroutines()
    {
        var cts = m_CoroutineCts;
        m_CoroutineCtsTerminated = true;
        m_CoroutineCts = null;
        if (cts == null) return;
        try { cts.Cancel(); }
        finally { cts.Dispose(); }
    }

    public virtual void OnAwake() { }
    public virtual void OnStart() { }
    public virtual void OnUpdate() { }
    public virtual void OnFixedUpdate() { }
    public virtual void OnDestroy() { }

    public virtual void OnApplicationStart() { }
    public virtual void OnApplicationPaused() { }
    public virtual void OnApplicationQuit() { }
    public virtual void OnFocusChanged(bool focused) { }

    public virtual void OnEnable() { }
    public virtual void OnDisable() { }

    public virtual void OnCollisionEnter2D(Collision2D collision) { }
    public virtual void OnCollisionStay2D(Collision2D collision) { }
    public virtual void OnCollisionExit2D(Collision2D collision) { }
}
