using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Index.Components;
using Index.Interop;
using Index.Jobs.Internal;

namespace Index.Jobs;

// IJobQuery — declarative ECS iteration backed by the native ref-API query
// view. The user writes a struct implementing IJobQuery with an `Execute`
// method whose parameter list IS the query:
//
//     [WithAll(typeof(NativePlayerTag))]
//     [WithoutAll(typeof(NativeFrozenTag))]
//     public struct MoveJob : IJobQuery
//     {
//         public float Dt;
//         public NativeArray<Vector2> Targets;   // side array indexed by rowIndex
//
//         public void Execute(int rowIndex,
//                             ref NativeTransform2D tr,
//                             in  NativeVelocity v)
//         {
//             tr.LocalPosition += v.Linear * Dt;
//             tr.LocalPosition = Vector2.MoveTowards(tr.LocalPosition, Targets[rowIndex], Dt);
//         }
//     }
//
//     JobHandle h = scene.ScheduleParallel(new MoveJob { ... });
//     h.Complete();
//
// Signature rules (validated on first Schedule per TJob):
//   - exactly one method named Execute, returning void
//   - optional leading `int rowIndex` parameter
//   - 1..8 component parameters, each `ref T` (write) or `in T` (read-only)
//     where T : unmanaged, IComponent
//   - no duplicate component types in the parameter list
//
// Filter rules (struct-level attributes):
//   - [WithAll(typeof(A), typeof(B))]    — entity must have these components
//   - [WithoutAll(typeof(C), typeof(D))] — entity must NOT have these
//   - [EnabledFilter(EnableMode.IncludeDisabled)] — override the default of
//                                                  enabled-only
//
// Iteration-stability contract: NO structural changes (Add/Remove/Destroy/
// Create on any of the query's component pools) inside Execute. The buffer
// is captured before the parallel dispatch — a structural change on one
// thread would invalidate pointers being read on another. Same rule as the
// existing QueryRef foreach, just stricter because the buffer outlives the
// calling thread for the duration of the parallel run.
public interface IJobQuery { }

public enum EnableMode
{
    IncludeDisabled = 0,
    EnabledOnly     = 1,
    DisabledOnly    = 2,
}

[AttributeUsage(AttributeTargets.Struct, AllowMultiple = false, Inherited = false)]
public sealed class WithAllAttribute : Attribute
{
    public WithAllAttribute(params Type[] types)
    {
        Types = types ?? Array.Empty<Type>();
    }

    public Type[] Types { get; }
}

[AttributeUsage(AttributeTargets.Struct, AllowMultiple = false, Inherited = false)]
public sealed class WithoutAllAttribute : Attribute
{
    public WithoutAllAttribute(params Type[] types)
    {
        Types = types ?? Array.Empty<Type>();
    }

    public Type[] Types { get; }
}

[AttributeUsage(AttributeTargets.Struct, AllowMultiple = false, Inherited = false)]
public sealed class EnabledFilterAttribute : Attribute
{
    public EnabledFilterAttribute(EnableMode mode)
    {
        Mode = mode;
    }

    public EnableMode Mode { get; }
}

// ──────────────────────────────────────────────────────────────────────
//  Internal: compiled per-type dispatch plan + IL-emitted invoker
// ──────────────────────────────────────────────────────────────────────

// One per concrete TJob. The IL-emitted body loads pointers from rowBase
// at compile-time-known offsets and calls Execute by ref — no boxing,
// no per-row reflection.
internal unsafe delegate void JobInvoker<TJob>(ref TJob job, IntPtr* rowBase, int rowIndex)
    where TJob : struct;

internal sealed class JobQueryPlan
{
    internal readonly string WriteNames;
    internal readonly string ReadonlyNames;
    internal readonly string MustHaveNames;
    internal readonly string WithoutNames;
    internal readonly int    EnableFilter;
    internal readonly int    PoolCount;
    internal readonly bool   HasRowIndexParam;
    internal readonly MethodInfo ExecuteMethod;

    // Component params in DECLARATION order. Native rowBase is laid out
    // as [writes-in-decl-order, reads-in-decl-order], so each entry here
    // also stores its slot index in rowBase.
    internal readonly ComponentParam[] Params;

    internal readonly struct ComponentParam
    {
        internal readonly Type Type;
        internal readonly bool IsWrite;
        internal readonly int  SlotInRowBase;

        internal ComponentParam(Type type, bool isWrite, int slot)
        {
            Type = type;
            IsWrite = isWrite;
            SlotInRowBase = slot;
        }
    }

    private JobQueryPlan(
        string writeNames, string readonlyNames, string mustHave, string without,
        int enableFilter, int poolCount, bool hasRowIndex,
        MethodInfo execute, ComponentParam[] parms)
    {
        WriteNames       = writeNames;
        ReadonlyNames    = readonlyNames;
        MustHaveNames    = mustHave;
        WithoutNames     = without;
        EnableFilter     = enableFilter;
        PoolCount        = poolCount;
        HasRowIndexParam = hasRowIndex;
        ExecuteMethod    = execute;
        Params           = parms;
    }

    internal static JobQueryPlan Build(Type jobType)
    {
        // ── Locate Execute ────────────────────────────────────────
        const BindingFlags flags =
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly;

        MethodInfo[] candidates = jobType.GetMethods(flags)
            .Where(m => m.Name == "Execute")
            .ToArray();

        if (candidates.Length == 0)
            throw new InvalidOperationException(
                $"IJobQuery struct '{jobType.FullName}' has no Execute method.");
        if (candidates.Length > 1)
            throw new InvalidOperationException(
                $"IJobQuery struct '{jobType.FullName}' has multiple Execute methods; exactly one is required.");

        MethodInfo execute = candidates[0];
        if (execute.ReturnType != typeof(void))
            throw new InvalidOperationException(
                $"{jobType.FullName}.Execute must return void.");

        // ── Parse parameter list ──────────────────────────────────
        ParameterInfo[] parms = execute.GetParameters();
        int firstCompIdx = 0;
        bool hasRowIndex = false;

        if (parms.Length > 0 && parms[0].ParameterType == typeof(int))
        {
            hasRowIndex = true;
            firstCompIdx = 1;
        }

        int compCount = parms.Length - firstCompIdx;
        if (compCount < 1)
            throw new InvalidOperationException(
                $"{jobType.FullName}.Execute must take at least one ref/in component parameter.");
        if (compCount > 8)
            throw new InvalidOperationException(
                $"{jobType.FullName}.Execute supports at most 8 component parameters (got {compCount}).");

        // First pass: classify each param as write or read, record element type.
        var paramTypes  = new Type[compCount];
        var paramWrites = new bool[compCount];
        var seen = new HashSet<Type>();

        for (int k = 0; k < compCount; k++)
        {
            ParameterInfo p = parms[firstCompIdx + k];

            if (!p.ParameterType.IsByRef)
                throw new InvalidOperationException(
                    $"{jobType.FullName}.Execute parameter '{p.Name}' must be ref or in.");

            Type elem = p.ParameterType.GetElementType()!;

            if (!elem.IsValueType)
                throw new InvalidOperationException(
                    $"{jobType.FullName}.Execute parameter '{p.Name}' must be a value type.");
            if (!typeof(IComponent).IsAssignableFrom(elem))
                throw new InvalidOperationException(
                    $"{jobType.FullName}.Execute parameter '{p.Name}' type '{elem.Name}' must implement IComponent.");
            if (!seen.Add(elem))
                throw new InvalidOperationException(
                    $"{jobType.FullName}.Execute parameter type '{elem.Name}' appears more than once.");

            // `in T` shows as IsByRef=true plus IsIn=true (the C# compiler
            // emits the `in` modreq + ParameterAttributes.In).
            paramTypes[k]  = elem;
            paramWrites[k] = !p.IsIn;

            // Force the layout-size check (one-time per T). This ensures any
            // C++/C# struct drift is caught on first Schedule rather than
            // mid-iteration when a pointer is dereferenced.
            Type componentTypes = typeof(ComponentTypes<>).MakeGenericType(elem);
            RuntimeHelpers.RunClassConstructor(componentTypes.TypeHandle);
        }

        // ── Build native name strings (writes first, reads after) ─
        var writeBuf    = new List<string>();
        var readBuf     = new List<string>();
        var slotOfParam = new int[compCount];
        int writeSlot = 0, readSlot = 0;
        int writeCount = paramWrites.Count(w => w);

        for (int k = 0; k < compCount; k++)
        {
            string name = ResolveRequiredNativeName(paramTypes[k], jobType);
            if (paramWrites[k])
            {
                slotOfParam[k] = writeSlot++;
                writeBuf.Add(name);
            }
            else
            {
                slotOfParam[k] = writeCount + readSlot++;
                readBuf.Add(name);
            }
        }

        var planParams = new ComponentParam[compCount];
        for (int k = 0; k < compCount; k++)
            planParams[k] = new ComponentParam(paramTypes[k], paramWrites[k], slotOfParam[k]);

        // ── Attribute-driven filters ──────────────────────────────
        string mustHave = JoinAttributeTypes(jobType.GetCustomAttribute<WithAllAttribute>()?.Types, jobType);
        string without  = JoinAttributeTypes(jobType.GetCustomAttribute<WithoutAllAttribute>()?.Types, jobType);

        EnableMode enable = jobType.GetCustomAttribute<EnabledFilterAttribute>()?.Mode ?? EnableMode.EnabledOnly;

        return new JobQueryPlan(
            writeNames:    string.Join('|', writeBuf),
            readonlyNames: string.Join('|', readBuf),
            mustHave:      mustHave,
            without:       without,
            enableFilter:  (int)enable,
            poolCount:     compCount,
            hasRowIndex:   hasRowIndex,
            execute:       execute,
            parms:         planParams);
    }

    private static string ResolveRequiredNativeName(Type componentType, Type jobType)
    {
        string? name = ComponentTypes.TryGetNativeName(componentType);
        if (string.IsNullOrEmpty(name))
            throw new InvalidOperationException(
                $"IJobQuery '{jobType.FullName}' references component type '{componentType.Name}' " +
                "which has no native registration. Register the component or add [NativeComponent(\"Native Name\")].");
        return name;
    }

    private static string JoinAttributeTypes(Type[]? types, Type jobType)
    {
        if (types == null || types.Length == 0) return "";

        var names = new List<string>(types.Length);
        foreach (Type t in types)
        {
            if (t == null) continue;
            if (!t.IsValueType || !typeof(IComponent).IsAssignableFrom(t))
                throw new InvalidOperationException(
                    $"IJobQuery '{jobType.FullName}' filter type '{t.Name}' must be a struct implementing IComponent.");

            string? n = ComponentTypes.TryGetNativeName(t);
            if (string.IsNullOrEmpty(n))
                throw new InvalidOperationException(
                    $"IJobQuery '{jobType.FullName}' filter component '{t.Name}' has no native registration.");
            names.Add(n);
        }
        return string.Join('|', names);
    }

    // Emit:
    //   void Invoker(ref TJob job, IntPtr* rowBase, int rowIndex) {
    //       job.Execute([rowIndex,]
    //                   ref *(T0*)rowBase[slot0],
    //                   ref *(T1*)rowBase[slot1],
    //                   ...);
    //   }
    internal Delegate BuildInvoker(Type delegateType, Type jobType)
    {
        var dm = new DynamicMethod(
            name:            $"IJobQuery_Invoke_{jobType.Name}",
            returnType:      typeof(void),
            parameterTypes:  new[] { jobType.MakeByRefType(), typeof(IntPtr*), typeof(int) },
            m:               typeof(JobQueryPlan).Module,
            skipVisibility:  true);

        ILGenerator il = dm.GetILGenerator();

        // receiver: ref TJob (arg 0)
        il.Emit(OpCodes.Ldarg_0);

        // optional leading int rowIndex (arg 2)
        if (HasRowIndexParam)
            il.Emit(OpCodes.Ldarg_2);

        // For each component parameter (in declaration order):
        //   load rowBase[slot] as a pointer and pass it as ref T.
        foreach (ComponentParam p in Params)
        {
            il.Emit(OpCodes.Ldarg_1);                         // IntPtr* rowBase
            il.Emit(OpCodes.Ldc_I4, p.SlotInRowBase * IntPtr.Size);
            il.Emit(OpCodes.Conv_I);                          // sign-extend to native int
            il.Emit(OpCodes.Add);                             // rowBase + slot*sizeof(IntPtr)
            il.Emit(OpCodes.Ldind_I);                         // load IntPtr stored there → T* as native int
            // The native int sitting on the stack is byref-compatible at the
            // call site for `ref T` / `in T` parameters. skipVisibility=true
            // disables the verifier that would otherwise reject this.
        }

        il.Emit(OpCodes.Call, ExecuteMethod);
        il.Emit(OpCodes.Ret);

        return dm.CreateDelegate(delegateType);
    }
}

internal static class JobQueryPlanFor<TJob> where TJob : struct, IJobQuery
{
    internal static readonly JobQueryPlan       Plan;
    internal static readonly JobInvoker<TJob>   Invoker;

    static JobQueryPlanFor()
    {
        Plan    = JobQueryPlan.Build(typeof(TJob));
        Invoker = (JobInvoker<TJob>)Plan.BuildInvoker(typeof(JobInvoker<TJob>), typeof(TJob));
    }
}

// ──────────────────────────────────────────────────────────────────────
//  Public scheduling surface
// ──────────────────────────────────────────────────────────────────────

public static class JobQueryExtensions
{
    // Parallel: rows are partitioned across JobSystem workers via
    // Job.ScheduleParallelFor. Each worker receives its own copy of the
    // job struct (matches Unity DOTS semantics — per-worker scratch fields
    // are safe; assume writes do NOT survive across rows on other threads).
    //
    // batchSize = 0 uses JobSystem.ComputeAutoBatchSize.
    public static JobHandle ScheduleParallel<TJob>(
        this Scene scene,
        TJob job,
        int batchSize = 0,
        CancellationToken cancellationToken = default)
        where TJob : struct, IJobQuery
    {
        return ScheduleInternal(scene, ref job, batchSize, parallel: true, cancellationToken);
    }

    // Single-job: the whole row range runs on one worker thread, off the
    // calling thread. Returns immediately with a JobHandle.
    public static JobHandle Schedule<TJob>(
        this Scene scene,
        TJob job,
        CancellationToken cancellationToken = default)
        where TJob : struct, IJobQuery
    {
        return ScheduleInternal(scene, ref job, batchSize: 0, parallel: false, cancellationToken);
    }

    private static unsafe JobHandle ScheduleInternal<TJob>(
        Scene scene,
        ref TJob job,
        int batchSize,
        bool parallel,
        CancellationToken cancellationToken)
        where TJob : struct, IJobQuery
    {
        if (scene == null)
            throw new ArgumentNullException(nameof(scene));
        if (string.IsNullOrEmpty(scene.Name))
            return default;

        JobQueryPlan        plan    = JobQueryPlanFor<TJob>.Plan;
        JobInvoker<TJob>    invoker = JobQueryPlanFor<TJob>.Invoker;

        var filters = new QueryRefFilters
        {
            SceneName     = scene.Name,
            WriteNames    = plan.WriteNames,
            ReadonlyNames = plan.ReadonlyNames,
            MustHaveNames = plan.MustHaveNames,
            WithoutNames  = plan.WithoutNames,
            EnableFilter  = plan.EnableFilter,
        };

        int poolCount = plan.PoolCount;

        // Pin first, then have native fill the pinned buffer DIRECTLY — saves a
        // full Array.Copy pass (~ 10K × poolCount × 8 bytes on the example shape)
        // and removes the intermediate thread-static array entirely from this
        // code path. The legacy thread-static buffer in QueryRefBuffers.Open is
        // still used by the non-job foreach `QueryRef<T>()` builder, which
        // consumes its result on the same thread and doesn't need pinning.
        //
        // Capacity heuristic: start at 64 rows. The pool buckets are power-of-2,
        // so this rounds up to a stable bucket (Length = 128 for 64×1 request,
        // 1024 for 64×8, etc.) — steady-state schedules of the same shape hit
        // the same hot bucket every frame and recycle the same pinned array.
        const int kInitialRowGuess = 64;
        JobQueryPinPool.PinnedBuffer pinBuf =
            JobQueryPinPool.Rent(kInitialRowGuess * poolCount);
        int rowCount;
        while (true)
        {
            int rowCap = pinBuf.Array.Length / poolCount;
            rowCount = InternalCalls.Scene_OpenQueryView(
                filters.SceneName, filters.WriteNames, filters.ReadonlyNames,
                filters.MustHaveNames, filters.WithoutNames, filters.EnableFilter,
                (void**)pinBuf.PinnedBase, rowCap);
            if (rowCount <= rowCap) break;
            // Undersize: return current buffer, rent fitted, retry. Rare path —
            // only happens when the matched-entity count grew past the previous
            // high-water mark since the last schedule of this shape.
            JobQueryPinPool.Return(pinBuf);
            pinBuf = JobQueryPinPool.Rent(rowCount * poolCount);
        }

        if (rowCount == 0)
        {
            JobQueryPinPool.Return(pinBuf);
            return default;
        }

        IntPtr pinnedBase = pinBuf.PinnedBase;

        // From this point on, ANY exception before the AsTask().ContinueWith
        // continuation is wired up must return the pin manually — otherwise
        // the pinned array leaks for the rest of the process. Guard the whole
        // schedule+attach with try/catch; the happy path returns inside the
        // continuation as before.
        JobHandle handle;
        try
        {
            // Capture by-value so the closures see a stable snapshot.
            TJob capturedJob = job;

            if (parallel)
            {
                // Range body — invoker fires per row inside a tight
                // managed loop, amortizing the delegate dispatch across
                // the whole batch instead of paying it per row.
                handle = Job.ScheduleParallelForRange(0, rowCount, (lo, hi) =>
                {
                    TJob local = capturedJob;
                    for (int row = lo; row < hi; row++)
                    {
                        IntPtr* rowBase = (IntPtr*)pinnedBase + (long)row * poolCount;
                        invoker(ref local, rowBase, row);
                    }
                }, batchSize, cancellationToken);
            }
            else
            {
                int rowCountLocal = rowCount;
                handle = Job.Schedule(() =>
                {
                    TJob local = capturedJob;
                    for (int r = 0; r < rowCountLocal; r++)
                    {
                        IntPtr* rowBase = (IntPtr*)pinnedBase + (long)r * poolCount;
                        invoker(ref local, rowBase, r);
                    }
                }, cancellationToken);
            }

            // Return the pinned buffer to the pool once the job completes
            // (success, fault, or cancellation — runs in all terminal
            // states). Ride synchronously on the worker so we don't add
            // ThreadPool latency. If AsTask() itself throws (custom
            // JobHandle impl), the outer catch returns the buffer.
            JobQueryPinPool.PinnedBuffer capturedPinBuf = pinBuf;
            handle.AsTask().ContinueWith(_ =>
            {
                JobQueryPinPool.Return(capturedPinBuf);
            }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
        }
        catch
        {
            JobQueryPinPool.Return(pinBuf);
            throw;
        }

        return handle;
    }
}
