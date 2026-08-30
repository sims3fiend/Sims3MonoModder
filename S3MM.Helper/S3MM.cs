using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace S3MM
{
    /// <summary>
    /// Current S3MM API version. Incremented only when the managed API shape changes (added/removed/renamed ICALLs, changed signatures, attribute semantics).
    /// Internal native changes that don't affect the managed surface do not bump this.
    /// </summary>
    public static class Version
    {
        public const int Api = 3;
    }

    /// <summary>Native bridge for S3MM hook system. These methods call into the native S3MM code.
    /// Internal: mods dispatch through <see cref="Hook"/> / <see cref="Chain"/>, never this class directly.</summary>
    internal static class Native
    {
        /// <summary>Chain-aware dispatch: calls the next hook, or the original if last. Prefer <see cref="Hook.CallNext"/>.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern object CallNext(uint methodToken, object thisPtr, object[] args);

        /// <summary>Bypasses all hooks and invokes the unpatched original directly. Prefer <see cref="Hook.CallOriginal"/>.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern object CallOriginal(uint methodToken, object thisPtr, object[] args);

        /// <summary>Walks interpreter frames to resolve the hooked method token for the current call. Returns 0 outside a hook context.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern uint GetCallerHookToken();

        /// <summary>True if the target behind <paramref name="methodToken"/> is an instance method. Lets the un-baked Chain.CallNext fallback split a leading-self arg from the params.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool TargetIsInstance(uint methodToken);

        // Caller introspection - each walks interpreter frames to the game frame below the innermost hooked method. See Hook.Caller*.
        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern IntPtr CallerMethod();

        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern string CallerName();

        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern bool CallerIs(string fullName);

        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern object CallerInstance();

        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern object[] CallerArgs();

        [MethodImpl(MethodImplOptions.InternalCall)]
        internal static extern uint CallerMintOffset();
    }

    /// <summary>
    /// Internal: late [PersistableStatic] registration for S3MM-loaded assemblies. Driven by native (persist_bridge.cpp), mods never call this.
    /// PersistStatic.GameplayInitialize scans the AppDomain once in StartupState, S3MM loads mods after that, so their statics never entered sPersistList.
    /// Same rules as vanilla ([assembly: PersistableStatic] gate, PersistStatic.IsFieldSerializable)
    /// Hot-reload: drops the old image's entries (matched by RuntimeFieldHandle VALUE, never by Assembly object identity - reflection wrappers are
    /// per-domain-cached and an object obtained on the game thread at startup is not reference-equal to one obtained on the sim thread later)
    /// and carries static values across. No Persist.TypeFound remap is needed: mono prepends to domain_assemblies, so GetTypeByUnqualifiedName
    /// already resolves unqualified type refs to the NEWEST image. Simulator.SignalAssembliesLoaded is fired so [Tunable] XML applies h-heh..
    /// </summary>
    internal static class PersistBridge
    {
        private const BindingFlags kStaticAny =
            BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
        private const BindingFlags kAnyInstance =
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;

        private static bool sInstalled;
        private static Type sAttrType; // Sims3.SimIFace.PersistableStaticAttribute
        private static ConstructorInfo sPersistVarCtor; // PersistStatic+PersistVariable(RuntimeFieldHandle)
        private static FieldInfo sListField; // PersistStatic.sPersistList : List<PersistVariable>, null = disabled
        private static FieldInfo sPvHandleField; // PersistVariable.mField : RuntimeFieldHandle
        private static MethodInfo sIsFieldSerializable; // PersistStatic.IsFieldSerializable(FieldInfo) : bool
        private static MethodInfo sSignalAssembliesLoaded; // Simulator.SignalAssembliesLoaded(Assembly[]), private

        // Hot-reload tracking: logical name -> newest image, and the handle values (MonoClassField*) we registered for it
        private static readonly Dictionary<string, Assembly> sLatestByMod =
            new Dictionary<string, Assembly>();
        private static readonly Dictionary<string, Dictionary<IntPtr, bool>> sHandlesByMod =
            new Dictionary<string, Dictionary<IntPtr, bool>>();
        private static readonly List<Assembly> sPendingSignal = new List<Assembly>();

        /// <summary>Resolve game internals and subscribe Persist.TypeFound. Once, after S3MM.Helper is loaded.</summary>
        internal static void Install()
        {
            if (sInstalled)
                return;
            sInstalled = true;
            try
            {
                Type persistStatic = Type.GetType("Sims3.SimIFace.PersistStatic, SimIFace", true);
                sAttrType = Type.GetType(
                    "Sims3.SimIFace.PersistableStaticAttribute, SimIFace",
                    true
                );
                Type persistVar = persistStatic.GetNestedType(
                    "PersistVariable",
                    BindingFlags.Public | BindingFlags.NonPublic
                );
                sListField = persistStatic.GetField("sPersistList", kStaticAny);
                sIsFieldSerializable = persistStatic.GetMethod(
                    "IsFieldSerializable",
                    kStaticAny,
                    null,
                    new Type[] { typeof(FieldInfo) },
                    null
                );
                if (persistVar == null || sListField == null || sIsFieldSerializable == null)
                    throw new MissingMemberException(
                        "PersistStatic internals not found (sPersistList / PersistVariable / IsFieldSerializable)"
                    );

                sPvHandleField = persistVar.GetField("mField", kAnyInstance);
                sPersistVarCtor = persistVar.GetConstructor(
                    kAnyInstance,
                    null,
                    new Type[] { typeof(RuntimeFieldHandle) },
                    null
                );
                if (sPvHandleField == null || sPersistVarCtor == null)
                    throw new MissingMemberException(
                        "PersistVariable layout changed (mField / ctor(RuntimeFieldHandle))"
                    );

                Type simulator = Type.GetType("Sims3.SimIFace.Simulator, SimIFace", true);
                sSignalAssembliesLoaded = simulator.GetMethod(
                    "SignalAssembliesLoaded",
                    kStaticAny,
                    null,
                    new Type[] { typeof(Assembly[]) },
                    null
                );
                if (sSignalAssembliesLoaded == null)
                    Log.Warn(
                        "[PersistBridge] Simulator.SignalAssembliesLoaded not found - [Tunable] XML will not apply to S3MM mods"
                    );

                Log.Info("[PersistBridge] Installed");
            }
            catch (Exception ex)
            {
                sListField = null;
                Log.Error(
                    "[PersistBridge] Install failed - [PersistableStatic] auto-registration disabled: "
                        + Describe(ex)
                );
            }
        }

        /// <summary>Register a loaded mod's fields and queue it for SignalAssembliesLoaded. A mod name seen before is a hot-reload: the previous image's entries are dropped and its statics carried across.</summary>
        internal static void OnModLoaded(string assemblyName, string modName)
        {
            if (sListField == null)
                return;
            Assembly asm = FindAssembly(assemblyName);
            if (asm == null)
            {
                Log.Warn(
                    "[PersistBridge] Assembly '"
                        + assemblyName
                        + "' not found in AppDomain - [PersistableStatic] fields of "
                        + modName
                        + " will not persist"
                );
                return;
            }
            Assembly prev;
            sLatestByMod.TryGetValue(modName, out prev);
            if (prev == asm)
                return; // already registered

            int removed = 0,
                carried = 0;
            if (prev != null)
            {
                removed = UnregisterMod(modName);
                carried = CarryStatics(prev, asm, modName);
            }
            sLatestByMod[modName] = asm;
            int added = RegisterAssembly(asm, modName);

            if (prev != null)
                Log.Info(
                    "[PersistBridge] "
                        + modName
                        + " reloaded: "
                        + removed
                        + " stale entries dropped, "
                        + carried
                        + " static values carried, "
                        + added
                        + " fields registered"
                );
            else if (added > 0)
                Log.Info(
                    "[PersistBridge] "
                        + modName
                        + ": registered "
                        + added
                        + " [PersistableStatic] field(s)"
                );
            sPendingSignal.Add(asm);
        }

        /// <summary>Simulator.SignalAssembliesLoaded for everything queued since the last flush (TuningControl applies [Tunable] XML here).</summary>
        internal static void FlushSignal()
        {
            if (sPendingSignal.Count == 0)
                return;
            Assembly[] batch = sPendingSignal.ToArray();
            sPendingSignal.Clear();
            if (sSignalAssembliesLoaded == null)
                return;
            try
            {
                sSignalAssembliesLoaded.Invoke(null, new object[] { batch });
                Log.Debug("[PersistBridge] Simulator.SignalAssembliesLoaded(" + batch.Length + ")");
            }
            catch (Exception ex)
            {
                // Subscriber threw (TuningControl rethrows GetTypes failures). Persistence unaffected
                Log.Warn("[PersistBridge] OnAssembliesLoaded subscriber threw: " + Describe(ex));
            }
        }

        // PersistStatic.GameplayInitialize's scan body for one assembly. Records the handle values so UnregisterMod can drop them on hot-reload
        private static int RegisterAssembly(Assembly asm, string modName)
        {
            if (asm.GetCustomAttributes(sAttrType, true).Length == 0)
            {
                Log.Debug(
                    "[PersistBridge] " + modName + ": no [assembly: PersistableStatic] - skipped"
                );
                return 0;
            }
            IList list = sListField.GetValue(null) as IList;
            if (list == null)
            {
                Log.Warn(
                    "[PersistBridge] PersistStatic.sPersistList is null (GameplayInitialize has not run) - skipping "
                        + modName
                );
                return 0;
            }
            Dictionary<IntPtr, bool> handles = new Dictionary<IntPtr, bool>();
            int added = 0;
            foreach (Type t in SafeGetTypes(asm, modName))
            {
                if (t == null)
                    continue;
                foreach (FieldInfo f in t.GetFields(kStaticAny))
                {
                    if (!(bool)sIsFieldSerializable.Invoke(null, new object[] { f }))
                        continue;
                    list.Add(sPersistVarCtor.Invoke(new object[] { f.FieldHandle }));
                    handles[f.FieldHandle.Value] = true;
                    added++;
                    Log.Debug("[PersistBridge] + " + t.FullName + "." + f.Name);
                }
            }
            sHandlesByMod[modName] = handles;
            return added;
        }

        // Drop the entries WE registered for this mod's previous image, matched by handle value.
        //  A RuntimeFieldHandle.Value is the raw MonoClassField* stable for the image's lifetime and thread/wrapper-independent, unlike Assembly reflection object identity
        private static int UnregisterMod(string modName)
        {
            Dictionary<IntPtr, bool> handles;
            if (!sHandlesByMod.TryGetValue(modName, out handles) || handles.Count == 0)
                return 0;
            IList list = sListField.GetValue(null) as IList;
            if (list == null)
                return 0;
            int removed = 0;
            for (int i = list.Count - 1; i >= 0; i--)
            {
                RuntimeFieldHandle h = (RuntimeFieldHandle)sPvHandleField.GetValue(list[i]);
                if (!handles.ContainsKey(h.Value))
                    continue;
                list.RemoveAt(i);
                removed++;
            }
            sHandlesByMod.Remove(modName);
            return removed;
        }

        // Copy [PersistableStatic] statics by type/field name. Values typed from the mod itself can't cross images (SetValue throws), that's what OnUnload/OnLoad is for
        private static int CarryStatics(Assembly oldAsm, Assembly newAsm, string modName)
        {
            int carried = 0;
            foreach (Type ot in SafeGetTypes(oldAsm, modName + " (previous image)"))
            {
                if (ot == null)
                    continue;
                Type nt = newAsm.GetType(ot.FullName, false);
                if (nt == null)
                    continue;
                foreach (FieldInfo of in ot.GetFields(kStaticAny | BindingFlags.DeclaredOnly))
                {
                    if (of.IsLiteral || of.GetCustomAttributes(sAttrType, true).Length == 0)
                        continue;
                    FieldInfo nf = nt.GetField(of.Name, kStaticAny | BindingFlags.DeclaredOnly);
                    if (nf == null || nf.IsLiteral)
                        continue;
                    try
                    {
                        nf.SetValue(null, of.GetValue(null));
                        carried++;
                    }
                    catch (Exception ex)
                    {
                        Log.Debug(
                            "[PersistBridge] "
                                + ot.FullName
                                + "."
                                + of.Name
                                + ": not carried ("
                                + Describe(ex)
                                + ")"
                        );
                    }
                }
            }
            return carried;
        }

        // Short name match. Hot-reload images get a unique assembly name (Mod_vN when recompiled, last char patched for in-memory loads), so this is unambiguous
        private static Assembly FindAssembly(string shortName)
        {
            if (shortName == null || shortName.Length == 0)
                return null;
            foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies())
            {
                if (new AssemblyName(a.FullName).Name == shortName)
                    return a;
            }
            return null;
        }

        private static Type[] SafeGetTypes(Assembly asm, string label)
        {
            try
            {
                return asm.GetTypes();
            }
            catch (ReflectionTypeLoadException ex)
            {
                Log.Warn(
                    "[PersistBridge] "
                        + label
                        + ": some types failed to load (missing dependency?) - scanning the rest"
                );
                return ex.Types ?? new Type[0];
            }
        }

        private static string Describe(Exception ex)
        {
            TargetInvocationException tie = ex as TargetInvocationException;
            if (tie != null && tie.InnerException != null)
                ex = tie.InnerException;
            return ex.GetType().Name + ": " + ex.Message;
        }
    }

    /// <summary>
    /// Typed chain forwarding - the DEFAULT way to call the next handler.
    /// Unlike <see cref="Hook.CallNext(object, object[])"/> (which boxes args into an object[] and dispatches at runtime), Chain.CallNext is a direct typed call that the native side rewrites to the next hook (or the original): no boxing, no allocation, no frame walk.
    ///
    /// Baking is AUTOMATIC, there is no opt-in flag, match the target's signature EXACTLY and the chain bakes (zero overhead). You will be baked and you will like it.
    /// If it can't bake (a boxing hook shares the method, a signature mismatch, etc.) the same call transparently falls back to boxing dispatch, so a by-value typed hook works either way
    /// Use <see cref="Audit.IsMethodBaked"/> / <see cref="Audit.GetBakeRejectReason"/> to see which path you got.
    /// CRASH QUARANTINE: if a baked target crashes the game natively twice across runs, it stops baking (typed hooks demote to boxing dispatch) until S3MM/baked_quarantine.txt is deleted - see GetBakeRejectReason and S3MM/baked_crashes.log for the why.
    /// Exception: Chain.ByRef only works baked - a hook using it is rejected rather than demoted (see <see cref="ByRef"/>).
    ///
    /// Call CallNext with the SAME signature as the hooked method - first type arg is the return type, the rest are the param types:
    /// <code>
    ///   [S3MM.Hook("Foo.Bar:Baz(int)")]
    ///   static int BazHook(int x) {
    ///       // prefix
    ///       int r = S3MM.Chain.CallNext&lt;int, int&gt;(x);   // R = int, then the param types
    ///       // postfix
    ///       return r;
    ///   }
    /// </code>
    /// CallNextVoid's type args are inferred from the arguments - <c>Chain.CallNextVoid(self, x)</c> compiles without spelling the generic list; only CallNext&lt;R, ..&gt; needs it (R can't be inferred).
    /// Arity cap: up to 8 forwarded parameters (9 generic args counting R) - use the boxing Hook.CallNext for a wider target.
    /// </summary>
    /// <remarks>
    /// INSTANCE targets (experimental): write the handler as a static leading-self method <c>static R Hook(object self, T0 a0, ...)</c> matching <c>instance R Target(T0 a0, ...)</c>.
    /// The receiver arrives as <c>self</c>; FORWARD it as the leading arg: <c>Chain.CallNext&lt;R, object, T0, ...&gt;(self, a0, ...)</c>.
    /// (The baked call op pops <c>this</c> off the eval stack for the instance target, so <c>self</c> must be passed, not omitted.)
    /// The receiver may be typed as the concrete class instead of <c>object</c> - any reference type is accepted (<c>static R Hook(Sim self, int x)</c> forwarding <c>Chain.CallNext&lt;R, Sim, int&gt;(self, x)</c>).
    /// Value-type (struct) receivers are not supported.
    /// </remarks>
    public static class Chain
    {
        private static readonly object[] EmptyArgs = new object[0];

        // BOXING FALLBACK - when a hook can't bake the call site isn't rewritten and this body runs: resolve the token by frame walk, box args, dispatch through Native.CallNext
        // TargetIsInstance decides whether to peel a leading self arg; only Chain.ByRef and a failed frame walk still throw (see ByRef / NotRewritten)
        private static object Dispatch(object[] all)
        {
            uint token = Native.GetCallerHookToken();
            if (token == 0)
                throw NotRewritten();
            if (Native.TargetIsInstance(token))
            {
                object self = all.Length > 0 ? all[0] : null;
                object[] rest;
                if (all.Length > 1)
                {
                    rest = new object[all.Length - 1];
                    System.Array.Copy(all, 1, rest, 0, all.Length - 1);
                }
                else
                {
                    rest = EmptyArgs;
                }
                return Native.CallNext(token, self, rest);
            }
            return Native.CallNext(token, null, all);
        }

        private static R Ret<R>(object result)
        {
            return result == null ? default(R) : (R)result;
        }

        // SAFETY NET - reached only when the call site couldn't be rewritten AND can't box-fallback: a Chain.ByRef site (the smuggled pointer needs the nop), or a frame-walk that found no hook context
        // Throw loudly rather than return default(R), which would silently turn the hook into a garbage-returning replacement
        private static Exception NotRewritten()
        {
            return new InvalidOperationException(
                "S3MM.Chain dispatch failed outside the baked path. A by-value Chain.CallNext falls back to boxing "
                    + "automatically; this throw means either a Chain.ByRef site (ref/out forwarding ONLY works on the "
                    + "baked fast path - fix the signature to bake, or use the boxing Hook.CallNext / Hook.Ref API), or "
                    + "the call was made outside a hook context (frame walk found no caller). See the S3MM log; "
                    + "Audit.GetBakeRejectReason gives the exact reason a typed hook didn't bake."
            );
        }

        // RETURNING - first type arg is the return type, the rest are parameter types
        [MethodImpl(MethodImplOptions.NoInlining)]
        public static R CallNext<R>()
        {
            return Ret<R>(Dispatch(EmptyArgs));
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static R CallNext<R, T0>(T0 a0)
        {
            return Ret<R>(Dispatch(new object[] { a0 }));
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static R CallNext<R, T0, T1>(T0 a0, T1 a1)
        {
            return Ret<R>(Dispatch(new object[] { a0, a1 }));
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static R CallNext<R, T0, T1, T2>(T0 a0, T1 a1, T2 a2)
        {
            return Ret<R>(Dispatch(new object[] { a0, a1, a2 }));
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static R CallNext<R, T0, T1, T2, T3>(T0 a0, T1 a1, T2 a2, T3 a3)
        {
            return Ret<R>(Dispatch(new object[] { a0, a1, a2, a3 }));
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static R CallNext<R, T0, T1, T2, T3, T4>(T0 a0, T1 a1, T2 a2, T3 a3, T4 a4)
        {
            return Ret<R>(Dispatch(new object[] { a0, a1, a2, a3, a4 }));
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static R CallNext<R, T0, T1, T2, T3, T4, T5>(
            T0 a0,
            T1 a1,
            T2 a2,
            T3 a3,
            T4 a4,
            T5 a5
        )
        {
            return Ret<R>(Dispatch(new object[] { a0, a1, a2, a3, a4, a5 }));
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static R CallNext<R, T0, T1, T2, T3, T4, T5, T6>(
            T0 a0,
            T1 a1,
            T2 a2,
            T3 a3,
            T4 a4,
            T5 a5,
            T6 a6
        )
        {
            return Ret<R>(Dispatch(new object[] { a0, a1, a2, a3, a4, a5, a6 }));
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static R CallNext<R, T0, T1, T2, T3, T4, T5, T6, T7>(
            T0 a0,
            T1 a1,
            T2 a2,
            T3 a3,
            T4 a4,
            T5 a5,
            T6 a6,
            T7 a7
        )
        {
            return Ret<R>(Dispatch(new object[] { a0, a1, a2, a3, a4, a5, a6, a7 }));
        }

        // VOID - parameter types only
        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void CallNextVoid()
        {
            Dispatch(EmptyArgs);
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void CallNextVoid<T0>(T0 a0)
        {
            Dispatch(new object[] { a0 });
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void CallNextVoid<T0, T1>(T0 a0, T1 a1)
        {
            Dispatch(new object[] { a0, a1 });
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void CallNextVoid<T0, T1, T2>(T0 a0, T1 a1, T2 a2)
        {
            Dispatch(new object[] { a0, a1, a2 });
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void CallNextVoid<T0, T1, T2, T3>(T0 a0, T1 a1, T2 a2, T3 a3)
        {
            Dispatch(new object[] { a0, a1, a2, a3 });
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void CallNextVoid<T0, T1, T2, T3, T4>(T0 a0, T1 a1, T2 a2, T3 a3, T4 a4)
        {
            Dispatch(new object[] { a0, a1, a2, a3, a4 });
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void CallNextVoid<T0, T1, T2, T3, T4, T5>(
            T0 a0,
            T1 a1,
            T2 a2,
            T3 a3,
            T4 a4,
            T5 a5
        )
        {
            Dispatch(new object[] { a0, a1, a2, a3, a4, a5 });
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void CallNextVoid<T0, T1, T2, T3, T4, T5, T6>(
            T0 a0,
            T1 a1,
            T2 a2,
            T3 a3,
            T4 a4,
            T5 a5,
            T6 a6
        )
        {
            Dispatch(new object[] { a0, a1, a2, a3, a4, a5, a6 });
        }

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void CallNextVoid<T0, T1, T2, T3, T4, T5, T6, T7>(
            T0 a0,
            T1 a1,
            T2 a2,
            T3 a3,
            T4 a4,
            T5 a5,
            T6 a6,
            T7 a7
        )
        {
            Dispatch(new object[] { a0, a1, a2, a3, a4, a5, a6, a7 });
        }

        /// <summary>
        /// BY-REF SMUGGLE for ref/out parameters. The compiler emits `ldloca; call ByRef&lt;T&gt;`, leaving a managed pointer on the stack
        /// The native side nops the ByRef call so that pointer flows straight into the rewritten call, giving true ref/out forwarding with zero overhead.
        /// Pass `ByRef(ref x)` as the corresponding CallNext argument.
        /// NOTE: ref/out forwarding works ONLY on the baked path - un-rewritten this body just throws, and object[] boxing can't carry a managed pointer. A ByRef hook that can't bake is rejected (use Hook.Ref for the boxing route).
        /// </summary>
        [MethodImpl(MethodImplOptions.NoInlining)]
        public static T ByRef<T>(ref T value)
        {
            throw NotRewritten();
        }
    }

    /// <summary>
    /// Fast native Mono lookup and field access API.
    /// Bypasses managed reflection overhead by calling cached native functions directly.
    /// All handles (IntPtr) are native Mono pointers, stable for the lifetime of the process.
    /// <code>
    ///   var simClass = S3MM.Cache.FindClass("Sims3GameplaySystems", "Sims3.Gameplay.Actors", "Sim");
    ///   var ageField = S3MM.Cache.FindField(simClass, "mAge");
    ///   object age = S3MM.Cache.GetField(someSim, ageField);
    /// </code>
    /// </summary>
    public static class Cache
    {
        /// <summary>Find a class by assembly name, namespace, and class name. Goes through the native mono_class_from_name cache. Falls back to ScriptHost::FindClass if assembly not found.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern IntPtr FindClass(
            string assemblyName,
            string nameSpace,
            string className
        );

        /// <summary>Find a field on a class by name. Goes through the native mono_class_get_field_from_name cache. Searches parent classes automatically.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern IntPtr FindField(IntPtr klass, string fieldName);

        /// <summary>Find a method on a class by name and parameter count. Goes through the native mono_class_get_method_from_name_flags cache.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern IntPtr FindMethod(IntPtr klass, string methodName, int paramCount);

        /// <summary>Get an instance field value from a managed object. Automatically boxes value types (int, float, bool, structs).</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern object GetField(object instance, IntPtr field);

        /// <summary>Set an instance field value on a managed object. Automatically unboxes value types.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetField(object instance, IntPtr field, object value);

        /// <summary>Get a static field value from a class. Automatically boxes value types.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern object GetStaticField(IntPtr klass, IntPtr field);

        /// <summary>Set a static field value on a class. Automatically unboxes value types.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticField(IntPtr klass, IntPtr field, object value);

        // Typed instance hell ahead

        /// <summary>Read an int field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern int GetFieldInt32(object instance, IntPtr field);

        /// <summary>Write an int field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldInt32(object instance, IntPtr field, int value);

        /// <summary>Read a uint field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern uint GetFieldUInt32(object instance, IntPtr field);

        /// <summary>Write a uint field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldUInt32(object instance, IntPtr field, uint value);

        /// <summary>Read a long field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern long GetFieldInt64(object instance, IntPtr field);

        /// <summary>Write a long field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldInt64(object instance, IntPtr field, long value);

        /// <summary>Read a ulong field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern ulong GetFieldUInt64(object instance, IntPtr field);

        /// <summary>Write a ulong field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldUInt64(object instance, IntPtr field, ulong value);

        /// <summary>Read a float field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern float GetFieldFloat(object instance, IntPtr field);

        /// <summary>Write a float field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldFloat(object instance, IntPtr field, float value);

        /// <summary>Read a double field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern double GetFieldDouble(object instance, IntPtr field);

        /// <summary>Write a double field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldDouble(object instance, IntPtr field, double value);

        /// <summary>Read a bool field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern bool GetFieldBool(object instance, IntPtr field);

        /// <summary>Write a bool field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldBool(object instance, IntPtr field, bool value);

        /// <summary>Read a byte field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern byte GetFieldByte(object instance, IntPtr field);

        /// <summary>Write a byte field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldByte(object instance, IntPtr field, byte value);

        /// <summary>Read an sbyte field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern sbyte GetFieldSByte(object instance, IntPtr field);

        /// <summary>Write an sbyte field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldSByte(object instance, IntPtr field, sbyte value);

        /// <summary>Read a short field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern short GetFieldInt16(object instance, IntPtr field);

        /// <summary>Write a short field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldInt16(object instance, IntPtr field, short value);

        /// <summary>Read a ushort field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern ushort GetFieldUInt16(object instance, IntPtr field);

        /// <summary>Write a ushort field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldUInt16(object instance, IntPtr field, ushort value);

        /// <summary>Read a char field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern char GetFieldChar(object instance, IntPtr field);

        /// <summary>Write a char field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetFieldChar(object instance, IntPtr field, char value);

        // Typed static field hell

        /// <summary>Read a static int field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern int GetStaticFieldInt32(IntPtr klass, IntPtr field);

        /// <summary>Write a static int field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldInt32(IntPtr klass, IntPtr field, int value);

        /// <summary>Read a static uint field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern uint GetStaticFieldUInt32(IntPtr klass, IntPtr field);

        /// <summary>Write a static uint field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldUInt32(IntPtr klass, IntPtr field, uint value);

        /// <summary>Read a static long field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern long GetStaticFieldInt64(IntPtr klass, IntPtr field);

        /// <summary>Write a static long field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldInt64(IntPtr klass, IntPtr field, long value);

        /// <summary>Read a static ulong field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern ulong GetStaticFieldUInt64(IntPtr klass, IntPtr field);

        /// <summary>Write a static ulong field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldUInt64(IntPtr klass, IntPtr field, ulong value);

        /// <summary>Read a static float field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern float GetStaticFieldFloat(IntPtr klass, IntPtr field);

        /// <summary>Write a static float field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldFloat(IntPtr klass, IntPtr field, float value);

        /// <summary>Read a static double field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern double GetStaticFieldDouble(IntPtr klass, IntPtr field);

        /// <summary>Write a static double field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldDouble(IntPtr klass, IntPtr field, double value);

        /// <summary>Read a static bool field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern bool GetStaticFieldBool(IntPtr klass, IntPtr field);

        /// <summary>Write a static bool field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldBool(IntPtr klass, IntPtr field, bool value);

        /// <summary>Read a static byte field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern byte GetStaticFieldByte(IntPtr klass, IntPtr field);

        /// <summary>Write a static byte field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldByte(IntPtr klass, IntPtr field, byte value);

        /// <summary>Read a static sbyte field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern sbyte GetStaticFieldSByte(IntPtr klass, IntPtr field);

        /// <summary>Write a static sbyte field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldSByte(IntPtr klass, IntPtr field, sbyte value);

        /// <summary>Read a static short field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern short GetStaticFieldInt16(IntPtr klass, IntPtr field);

        /// <summary>Write a static short field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldInt16(IntPtr klass, IntPtr field, short value);

        /// <summary>Read a static ushort field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern ushort GetStaticFieldUInt16(IntPtr klass, IntPtr field);

        /// <summary>Write a static ushort field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldUInt16(IntPtr klass, IntPtr field, ushort value);

        /// <summary>Read a static char field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern char GetStaticFieldChar(IntPtr klass, IntPtr field);

        /// <summary>Write a static char field directly, no boxing.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void SetStaticFieldChar(IntPtr klass, IntPtr field, char value);

        // Invocation

        /// <summary>Invoke a method handle on a target object with arguments. Automatically unboxes value-type arguments and boxes value-type returns.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern object Invoke(IntPtr method, object instance, object[] args);

        /// <summary>Typed convenience wrapper for Invoke. Casts return value to T, returning default(T) if null.</summary>
        public static T Invoke<T>(IntPtr method, object instance, object[] args)
        {
            object result = Invoke(method, instance, args);
            if (result == null)
                return default(T);
            return (T)result;
        }

        // BY-NAME conveniences - one-line composition over the cached Find* ICALLs
        // Each call costs two extra ICALL hops over a raw handle (the lookups hit the native per-(class,name) caches, never a scan), so these are fine for event-ish code; keep raw handles for per-frame hot paths
        // Missing members THROW ArgumentException so a typo'd name is loud at the call site - a null return from GetField always means "the field's value is null", never "field not found"

        /// <summary>Read an instance field by name. The class is resolved from the object's runtime type; parent classes are searched. Throws ArgumentException if no such field exists.</summary>
        public static object GetField(object instance, string fieldName)
        {
            if (instance == null)
                throw new ArgumentNullException("instance");
            IntPtr field = FindField(GetObjectClass(instance), fieldName);
            if (field == IntPtr.Zero)
                throw new ArgumentException("Field not found: " + fieldName, "fieldName");
            return GetField(instance, field);
        }

        /// <summary>Write an instance field by name (see <see cref="GetField(object, string)"/> for resolution rules). Throws ArgumentException if no such field exists.</summary>
        public static void SetField(object instance, string fieldName, object value)
        {
            if (instance == null)
                throw new ArgumentNullException("instance");
            IntPtr field = FindField(GetObjectClass(instance), fieldName);
            if (field == IntPtr.Zero)
                throw new ArgumentException("Field not found: " + fieldName, "fieldName");
            SetField(instance, field, value);
        }

        /// <summary>Read a static field by name from a class handle (from <see cref="FindClass"/>). Throws ArgumentException if no such field exists.</summary>
        public static object GetStaticField(IntPtr klass, string fieldName)
        {
            IntPtr field = FindField(klass, fieldName);
            if (field == IntPtr.Zero)
                throw new ArgumentException("Field not found: " + fieldName, "fieldName");
            return GetStaticField(klass, field);
        }

        /// <summary>Write a static field by name from a class handle. Throws ArgumentException if no such field exists.</summary>
        public static void SetStaticField(IntPtr klass, string fieldName, object value)
        {
            IntPtr field = FindField(klass, fieldName);
            if (field == IntPtr.Zero)
                throw new ArgumentException("Field not found: " + fieldName, "fieldName");
            SetStaticField(klass, field, value);
        }

        /// <summary>
        /// Invoke an instance method by name.
        /// Resolution is by name + ARGUMENT COUNT (FindMethod); for overloads that differ only by parameter types, resolve the handle explicitly and use Invoke.
        /// Throws ArgumentException if no matching method exists.
        /// </summary>
        public static object Call(object instance, string methodName, params object[] args)
        {
            if (instance == null)
                throw new ArgumentNullException("instance");
            int argCount = args != null ? args.Length : 0;
            IntPtr method = FindMethod(GetObjectClass(instance), methodName, argCount);
            if (method == IntPtr.Zero)
                throw new ArgumentException(
                    "Method not found: " + methodName + "(" + argCount + " args)",
                    "methodName"
                );
            return Invoke(method, instance, args);
        }

        /// <summary>Typed <see cref="Call(object, string, object[])"/>. Casts the result to T, default(T) if null.</summary>
        public static T Call<T>(object instance, string methodName, params object[] args)
        {
            object result = Call(instance, methodName, args);
            if (result == null)
                return default(T);
            return (T)result;
        }

        /// <summary>Invoke a static method by name on a class handle (name + argument count resolution, same caveats as <see cref="Call(object, string, object[])"/>).</summary>
        public static object CallStatic(IntPtr klass, string methodName, params object[] args)
        {
            int argCount = args != null ? args.Length : 0;
            IntPtr method = FindMethod(klass, methodName, argCount);
            if (method == IntPtr.Zero)
                throw new ArgumentException(
                    "Method not found: " + methodName + "(" + argCount + " args)",
                    "methodName"
                );
            return Invoke(method, null, args);
        }

        /// <summary>Typed <see cref="CallStatic(IntPtr, string, object[])"/>. Casts the result to T, default(T) if null.</summary>
        public static T CallStatic<T>(IntPtr klass, string methodName, params object[] args)
        {
            object result = CallStatic(klass, methodName, args);
            if (result == null)
                return default(T);
            return (T)result;
        }

        // Nested class lookup

        /// <summary>Find a nested class within a parent class. Useful for accessing private nested types that managed reflection cannot reach.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern IntPtr FindNestedClass(IntPtr parentKlass, string nestedClassName);

        // Object / Class introspection

        /// <summary>Get the MonoClass* handle for any managed object's runtime type.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern IntPtr GetObjectClass(object obj);

        /// <summary>Get the class name from a MonoClass* handle.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string GetClassName(IntPtr klass);

        /// <summary>Get the namespace from a MonoClass* handle. Returns null for classes with no namespace.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string GetClassNamespace(IntPtr klass);

        /// <summary>Get the parent (base) class of a MonoClass* handle. Returns IntPtr.Zero for System.Object.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern IntPtr GetParentClass(IntPtr klass);

        /// <summary>Check whether a MonoClass* represents an enum type.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern bool IsEnum(IntPtr klass);

        /// <summary>Get the fully-qualified method name from a MonoMethod* handle. Format: "Namespace.Class::MethodName"</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string GetMethodName(IntPtr method);
    }

    /// <summary>
    /// Auto-prefixed logging API. The mod's assembly name is resolved automatically from interpreter frames no need to pass "[ModName]" manually.
    /// Messages appear in the S3MM log file and the in-game Mods panel.
    /// </summary>
    /// <example>
    /// S3MM.Log.Info("Initialized successfully");   // -> [INFO] [MyMod] Initialized successfully
    /// S3MM.Log.Warn("Field not found: mAge");      // -> [WARN] [MyMod] Field not found: mAge
    /// S3MM.Log.Error("Hook failed: " + ex.Message);
    /// </example>
    public static class Log
    {
        /// <summary>Log an informational message.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void Info(string message);

        /// <summary>Log a warning message.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void Warn(string message);

        /// <summary>Log an error message.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void Error(string message);

        /// <summary>Log a debug message (only visible when debug logging is enabled).</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void Debug(string message);

        /// <summary>
        /// Write a string to the S3MM log file as a data channel with no length cap
        /// One call produces one line: "[ModName] " followed by the string, plus a trailing newline. Not shown in the Mods panel.
        /// Note: the string must not contain embedded newlines if you need line-based reassembly.
        /// </summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void Raw(string message);
    }

    /// <summary>
    /// Marks a static void parameterless method as an OnLoad handler.
    /// Called AFTER all hooks for the mod are registered and applied.
    /// Use this for initialization that depends on hooks being active, or for one-time setup like caching field handles.
    /// Safer than static constructors: exceptions are caught and logged with mod name attribution instead of causing a permanent TypeInitializationException.
    /// Multiple OnLoad methods per mod are supported. Also fires after hot-reload (after new hooks are installed).
    /// </summary>
    /// <example>
    /// [S3MM.OnLoad]
    /// public static void OnLoad()
    /// {
    ///     _lotClass = S3MM.Cache.FindClass("Sims3GameplaySystems",
    ///         "Sims3.Gameplay.Core", "Lot");
    ///     _tickField = S3MM.Cache.FindField(_lotClass, "mSimulateTicks");
    ///     S3MM.Log.Info("Initialized");
    /// }
    /// </example>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    public class OnLoadAttribute : Attribute { }

    /// <summary>
    /// Marks a static void parameterless method as an OnUnload handler.
    /// Called before hooks are removed during hot-reload, giving the mod a chance to restore modified game state (static fields, etc.).
    /// Multiple OnUnload methods per mod are supported.
    /// </summary>
    /// <example>
    /// [S3MM.OnUnload]
    /// public static void OnUnload()
    /// {
    ///     S3MM.Cache.SetStaticField(myClass, myField, savedValue);
    ///     S3MM.Log.Info("Cleaned up!");
    /// }
    /// </example>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    public class OnUnloadAttribute : Attribute { }

    internal static class TargetParser
    {
        /// <summary>
        /// Parses "Namespace.Class:Method(args)" or "Namespace.Class.Method(args)" format.
        /// Returns (targetClass, targetMethod, signature).
        /// </summary>
        public static void Parse(
            string fullTarget,
            out string targetClass,
            out string targetMethod,
            out string signature
        )
        {
            // Extract optional signature: "(Type1,Type2)"
            int parenStart = fullTarget.IndexOf('(');
            int parenEnd = fullTarget.LastIndexOf(')');
            if (parenStart > 0 && parenEnd > parenStart)
            {
                signature = fullTarget.Substring(parenStart, parenEnd - parenStart + 1);
                fullTarget = fullTarget.Substring(0, parenStart);
            }
            else
            {
                signature = null;
            }

            // Prefer colon separator: "Namespace.Class:Method"
            int colonPos = fullTarget.LastIndexOf(':');
            if (colonPos > 0)
            {
                targetClass = fullTarget.Substring(0, colonPos);
                targetMethod = fullTarget.Substring(colonPos + 1);
            }
            else
            {
                // Dot separator "Namespace.Class.Method": split on last dot
                int lastDot = fullTarget.LastIndexOf('.');
                if (lastDot > 0)
                {
                    targetClass = fullTarget.Substring(0, lastDot);
                    targetMethod = fullTarget.Substring(lastDot + 1);
                }
                else
                {
                    targetClass = "";
                    targetMethod = fullTarget;
                }
            }
        }
    }

    /// <summary>
    /// Marks a method as a hook. The hook runs in place of the original method.
    /// Forward via S3MM.Chain.CallNext (typed, bakes when it can) or S3MM.Hook.CallNext (boxing) to invoke the next hook in the chain or the original.
    /// Code before CallNext = "prefix" behavior, code after = "postfix" behavior. Not calling CallNext effectively replaces the original method.
    ///
    /// IMPORTANT - your handler's return type IS the return ABI: there is no marshalling layer, whatever you return is written straight into the slot the game reads.
    /// Declare the target's EXACT return type - declaring 'object' on a target that returns an enum or struct hands the caller a boxed-object pointer where it expected the value, which usually shows up as a pointer-sized number (0x4xxxxxxx) rather than a crash. Reference-type returns (string, class) are unaffected.
    /// A shape-mismatched ENTRY hook (the highest-priority hook on a target) is now refused at install time with an ERROR naming the mismatch - the target stays unhooked rather than corrupting its callers. Loosely-typed catch-all handlers remain legal at lower priority behind a shape-matched entry hook.
    /// Value-type returns of ANY width are supported when declared exactly (the runtime pre-boxes the return; a 16-byte struct round-trips intact). Value types as PARAMETERS are fine at any size.
    /// </summary>
    /// <example>
    /// [S3MM.Hook("Sims3.Gameplay.Sim:DoThing(int,string)")]  // With signature for overloads
    /// [S3MM.Hook("Sims3.Gameplay.Sim:DoThing")]              // Any overload
    /// [S3MM.Hook("Sims3.Gameplay.Sim.DoThing")]              // Dot syntax also works
    ///
    /// // Return type matches the target exactly - NOT object:
    /// [S3MM.Hook("Sims3.Gameplay.Core.Lot:GetMetaAutonomyVenueType")]
    /// public static MetaAutonomyVenueType HookVenueType(object self) {
    ///     return S3MM.Chain.CallNext&lt;MetaAutonomyVenueType, object&gt;(self);
    /// }
    /// </example>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    public class HookAttribute : Attribute
    {
        private string _targetClass;
        private string _targetMethod;
        private string _signature; // Optional: "(int,string)" for overload resolution
        private int _priority;

        public string TargetClass
        {
            get { return _targetClass; }
        }
        public string TargetMethod
        {
            get { return _targetMethod; }
        }
        public string Signature
        {
            get { return _signature; }
        }
        public int Priority
        {
            get { return _priority; }
            set { _priority = value; }
        }

        /// <summary>Target format: "Namespace.Class:Method(args)" or "Namespace.Class.Method(args)". Signature is optional, for overload resolution.</summary>
        public HookAttribute(string fullTarget)
        {
            TargetParser.Parse(fullTarget, out _targetClass, out _targetMethod, out _signature);
            _priority = 0;
        }
    }

    /// <summary>
    /// Marks a method as a read-only watcher. Watchers observe method calls without modifying behavior, they run FIRST (before all hooks) with priority 1000000 + user priority.
    /// Best practice to always call Hook.CallNext() to let the chain continue, return the unmodified result, use for logging/metrics/debugging.
    ///
    /// IMPORTANT - a watcher sits in the return path like any other hook, so the return-type rules of <see cref="HookAttribute"/> apply unchanged: declare the target's EXACT return type, never 'object'.
    /// "Read-only" describes your intent, not an exemption from the return contract.
    /// </summary>
    /// <example>
    /// // Target returns void - watcher returns void:
    /// [S3MM.Watcher("Sims3.Gameplay.Sim:DoThing")]
    /// public static void WatchDoThing(Sim __instance) {
    ///     Logger.Log("DoThing was called!");
    ///     Hook.CallNext(__instance);
    /// }
    ///
    /// // Target returns an enum - watcher returns that enum, and passes the value through untouched:
    /// [S3MM.Watcher("Sims3.Gameplay.Core.Lot:GetMetaAutonomyVenueType")]
    /// public static MetaAutonomyVenueType WatchVenueType(object self) {
    ///     MetaAutonomyVenueType result = S3MM.Chain.CallNext&lt;MetaAutonomyVenueType, object&gt;(self);
    ///     Logger.Log("venue type: " + result);
    ///     return result;
    /// }
    /// </example>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    public class WatcherAttribute : Attribute
    {
        private string _targetClass;
        private string _targetMethod;
        private string _signature;
        private int _priority;

        public string TargetClass
        {
            get { return _targetClass; }
        }
        public string TargetMethod
        {
            get { return _targetMethod; }
        }
        public string Signature
        {
            get { return _signature; }
        }

        /// <summary>Ordering among watchers on the same target (higher runs first). The effective chain priority is 1000000 + this, so watchers always precede hooks. Usage: [S3MM.Watcher("...", Priority = 5)]</summary>
        public int Priority
        {
            get { return _priority; }
            set { _priority = value; }
        }

        /// <summary>Target format: "Namespace.Class:Method(args)" or "Namespace.Class.Method(args)". Signature is optional, for overload resolution.</summary>
        public WatcherAttribute(string fullTarget)
        {
            TargetParser.Parse(fullTarget, out _targetClass, out _targetMethod, out _signature);
            _priority = 0;
        }
    }

    /// <summary>
    /// BOXING-PATH helper + registration surface for hook methods.
    /// CallNext/CallOriginal here box args into an object[] and dispatch at runtime - the universal fallback for handlers whose signature deliberately doesn't match the target (catch-all object self, dynamic targets).
    /// For a signature-matching handler prefer <see cref="Chain"/>, which is typed and bakes to zero-overhead dispatch when it can.
    /// </summary>
    // TODO: Could add a cross-hook state bag (Dictionary<string,object>) here if a real use case comes up - local variables already survive across CallNext since the managed frame stays alive, so none yet
    public static class Hook
    {
        /// <summary>
        /// Calls the next handler in the hook chain, or the original method if this is the last hook.
        /// Always resolves the token by walking interpreter frames for reliability.
        /// Throws InvalidOperationException when called outside a hook context - fails loudly rather than silently returning null.
        /// </summary>
        public static object CallNext(object instance, params object[] args)
        {
            uint token = Native.GetCallerHookToken();
            if (token == 0)
                throw new InvalidOperationException(
                    "S3MM.Hook.CallNext: no hook context (frame walk found no caller). Call this only from inside a hook handler."
                );

            return Native.CallNext(token, instance, args);
        }

        /// <summary>
        /// Calls the true original method, skipping all hooks in the chain.
        /// Always resolves the token by walking interpreter frames for reliability.
        /// Throws InvalidOperationException when called outside a hook context (see <see cref="CallNext(object, object[])"/>).
        /// </summary>
        // TODO: Same as CallNext, verify native retry covers first-call bug fully.
        public static object CallOriginal(object instance, params object[] args)
        {
            uint token = Native.GetCallerHookToken();
            if (token == 0)
                throw new InvalidOperationException(
                    "S3MM.Hook.CallOriginal: no hook context (frame walk found no caller). Call this only from inside a hook handler."
                );

            return Native.CallOriginal(token, instance, args);
        }

        /// <summary>Calls the next handler in the chain and returns a typed result.</summary>
        public static T CallNext<T>(object instance, params object[] args)
        {
            object result = CallNext(instance, args);
            if (result == null)
                return default(T);
            return (T)result;
        }

        /// <summary>Calls the true original method (skipping all hooks) and returns a typed result.</summary>
        public static T CallOriginal<T>(object instance, params object[] args)
        {
            object result = CallOriginal(instance, args);
            if (result == null)
                return default(T);
            return (T)result;
        }

        /// <summary>
        /// Wraps a value type for byref (ref/out) parameter passing to CallNext.
        /// When hooking methods with ref or out parameters, the original method needs to modify the caller's storage.
        /// Use this helper to create a single-element array that the native layer will pass by reference, after CallNext returns, read the modified value from index [0] and copy it back to your ref parameter.
        ///
        /// NOTE: The native layer applies the byref treatment ONLY at argument positions where the TARGET parameter is ref/out, so a legitimate single-element array passed to a by-value array parameter is never misinterpreted.
        /// The struct constraint stays as a managed-side guardrail against wrapping by mistake.
        /// For reference-type ref params (e.g. ref uint[]), manually create a single-element array: new uint[1][] { idArray }.
        /// </summary>
        /// <example>
        /// [S3MM.Hook("SomeClass:TryGetValue(int,int&amp;)")]
        /// public static bool TryGetValueHook(object self, int key, ref int value)
        /// {
        ///     var valueRef = Hook.Ref(value);
        ///     bool result = Hook.CallNext&lt;bool&gt;(self, key, valueRef);
        ///     value = valueRef[0];  // Copy back modified value
        ///     return result;
        /// }
        /// </example>
        public static T[] Ref<T>(T value)
            where T : struct
        {
            return new T[] { value };
        }

        // CALLER INTROSPECTION - who called the hooked method. Each member re-walks the interpreter frames (cheap, no capture), so read what you need and nothing more, please.
        // Properties are allocation-free (CallerName allocates its string), GetCallerArgs boxes a copy of every parameter and is meant for diagnostics, not hot paths/hooks.
        // The caller of the INNERMOST hooked method below you. A hooked method called from another hook's handler reports that handler, a recursive target reports the outer activation.
        // Native/engine callers (simulator tasks, event dispatch, BeginInvoke targets) have no managed frame and report IntPtr.Zero / null / false.
        // Values are a live view of the caller's frame, not a snapshot at call time.

        /// <summary>Canonical MonoMethod* of the calling method, or IntPtr.Zero for a native caller or outside a hook context. Stable for the process, usable with <see cref="RegisterByPointer"/>.</summary>
        public static IntPtr CallerMethod
        {
            get { return Native.CallerMethod(); }
        }

        /// <summary>"Namespace.Class::Method" of the caller, or null.</summary>
        public static string CallerName
        {
            get { return Native.CallerName(); }
        }

        /// <summary>True if the caller is <paramref name="method"/> (from <see cref="Cache.FindMethod"/> or a previous <see cref="CallerMethod"/>). Pointer compare, no allocation - the cheapest gate.</summary>
        public static bool CalledFrom(IntPtr method)
        {
            return method != IntPtr.Zero && Native.CallerMethod() == method;
        }

        /// <summary>True if the caller's full name is <paramref name="fullName"/> ("Namespace.Class::Method"). Compared natively, no managed allocation.</summary>
        public static bool CalledFrom(string fullName)
        {
            return Native.CallerIs(fullName);
        }

        /// <summary>The caller's <c>this</c>, or null for a static method or native caller. Struct receivers come back as a boxed copy.</summary>
        public static object CallerInstance
        {
            get { return Native.CallerInstance(); }
        }

        /// <summary>The caller's current parameter values, boxed, in signature order (ref/out params dereferenced). Null when there is no managed caller or the frame can't be read.</summary>
        public static object[] GetCallerArgs()
        {
            return Native.CallerArgs();
        }

        /// <summary>MINT bytecode offset of the call site inside the caller. Distinguishes two call sites in one method; it is not an IL offset and changes if the method is re-transformed.</summary>
        public static uint CallerMintOffset
        {
            get { return Native.CallerMintOffset(); }
        }

        // TODO: Resort
        /// <summary>
        /// Register a hook by raw method pointers. Use Cache.FindMethod() or MethodBase.MethodHandle.Value to obtain the IntPtr.
        /// hookType: 0 = Hook, 1 = Watcher.
        /// </summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void RegisterByPointer(
            IntPtr targetMethod,
            IntPtr hookMethod,
            int hookType,
            int priority
        );

        /// <summary>
        /// Commit hook registrations made after initial load without a mod reload.
        /// Reinstalls chains whose hook set changed (recompiling baked chains in place) and installs any new chains.
        /// Use after calling RegisterByPointer at runtime.
        /// </summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void ApplyPending();

        /// <summary>
        /// Convenience: register a hook given MethodBase objects.
        /// Uses MethodHandle.Value to get the native MonoMethod* pointer.
        /// </summary>
        public static void Register(
            System.Reflection.MethodBase target,
            System.Reflection.MethodBase hook
        )
        {
            RegisterByPointer(target.MethodHandle.Value, hook.MethodHandle.Value, 0, 0);
        }

        /// <summary>
        /// Convenience: register a hook given MethodBase objects with priority.
        /// </summary>
        public static void Register(
            System.Reflection.MethodBase target,
            System.Reflection.MethodBase hook,
            int priority
        )
        {
            RegisterByPointer(target.MethodHandle.Value, hook.MethodHandle.Value, 0, priority);
        }

        /// <summary>
        /// Convenience: register a watcher given MethodBase objects.
        /// </summary>
        public static void RegisterWatcher(
            System.Reflection.MethodBase target,
            System.Reflection.MethodBase hook
        )
        {
            RegisterByPointer(target.MethodHandle.Value, hook.MethodHandle.Value, 1, 0);
        }

        /// <summary>
        /// Convenience: register a watcher given MethodBase objects with priority.
        /// </summary>
        public static void RegisterWatcher(
            System.Reflection.MethodBase target,
            System.Reflection.MethodBase hook,
            int priority
        )
        {
            RegisterByPointer(target.MethodHandle.Value, hook.MethodHandle.Value, 1, priority);
        }

        // MIXED-FORM conveniences - the dominant runtime-registration shape is an IntPtr target from Cache.FindMethod plus a reflected handler
        // These wrap RegisterByPointer so mods never spell the raw hookType int (0 = hook, 1 = watcher)

        /// <summary>Register a hook on a native method handle (e.g. from <see cref="Cache.FindMethod"/>) with a reflected handler.</summary>
        public static void Register(IntPtr targetMethod, System.Reflection.MethodBase hook)
        {
            RegisterByPointer(targetMethod, hook.MethodHandle.Value, 0, 0);
        }

        /// <summary>Register a hook on a native method handle with a reflected handler and priority.</summary>
        public static void Register(
            IntPtr targetMethod,
            System.Reflection.MethodBase hook,
            int priority
        )
        {
            RegisterByPointer(targetMethod, hook.MethodHandle.Value, 0, priority);
        }

        /// <summary>Register a watcher on a native method handle with a reflected handler.</summary>
        public static void RegisterWatcher(IntPtr targetMethod, System.Reflection.MethodBase hook)
        {
            RegisterByPointer(targetMethod, hook.MethodHandle.Value, 1, 0);
        }

        /// <summary>Register a watcher on a native method handle with a reflected handler and priority.</summary>
        public static void RegisterWatcher(
            IntPtr targetMethod,
            System.Reflection.MethodBase hook,
            int priority
        )
        {
            RegisterByPointer(targetMethod, hook.MethodHandle.Value, 1, priority);
        }
    }

    /// <summary>Describes a single hook or watcher entry in a hook chain.</summary>
    public struct HookInfo
    {
        /// <summary>Fully-qualified target in "Namespace.Class:Method" format.</summary>
        public string Target;

        /// <summary>Name of the mod that registered this hook.</summary>
        public string ModName;

        /// <summary>Priority value (lower = runs first for hooks, watchers always run before hooks).</summary>
        public int Priority;

        /// <summary>True if this is a read-only watcher, false if it's a hook.</summary>
        public bool IsWatcher;

        /// <summary>
        /// True for the chain's entry hook - the method body callers actually dispatch into.
        /// The entry can starve every lower-priority hook by returning without calling CallNext, so when a hook
        /// "isn't firing", check which mod holds the entry before suspecting the install.
        /// </summary>
        public bool IsEntry;
    }

    /// <summary>Describes a core assembly that was replaced by a mod.</summary>
    public struct ReplacedAssembly
    {
        /// <summary>Short assembly name, e.g. "ScriptCore".</summary>
        public string AssemblyName;

        /// <summary>Path to the .package file that provided the replacement, or empty if unknown.</summary>
        public string SourcePackage;
    }

    /// <summary>
    /// Read-only introspection API for querying S3MM hook chains, assembly conflicts, and loaded mods.
    /// Tier 1 methods (<see cref="IsMethodHooked"/>, <see cref="IsAssemblyReplaced"/>) are optimized for per-frame or per-exception use.
    /// Tier 2 methods return typed arrays directly from native, with no string parsing overhead.
    /// </summary>
    /// <example>
    /// if (S3MM.Audit.IsMethodHooked("Sims3.Gameplay.Actors.Sim:DoThing"))
    ///     annotation += " [HOOKED by S3MM]";
    ///
    /// // Dump all hooks:
    /// HookInfo[] hooks = S3MM.Audit.GetAllHooks();
    /// foreach (var h in hooks)
    ///     Log.Info(h.Target + " hooked by " + h.ModName);
    /// </example>
    public static class Audit
    {
        // T1: Fast checks (ICALLs, safe for hot paths)

        /// <summary>
        /// Returns true if the specified method has any S3MM hooks installed.
        /// </summary>
        /// <param name="classMethod">Fully-qualified method in "Namespace.Class:Method" format, e.g. "Sims3.Gameplay.Actors.Sim:DoThing".</param>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern bool IsMethodHooked(string classMethod);

        /// <summary>
        /// Returns true if the method's hook chain dispatches via the zero-overhead BAKED path (typed Chain.CallNext), as opposed to the boxing path.
        /// Use it to confirm a typed hook actually landed on the fast path:
        /// <code>
        ///   IsMethodHooked == false               -> not hooked (or the only hook used Chain.ByRef and was stripped)
        ///   IsMethodHooked &amp;&amp; !IsMethodBaked  -> boxing dispatch (object[] args, runtime frame walk)
        ///   IsMethodHooked &amp;&amp; IsMethodBaked   -> baked fast path (direct typed forwarding, no boxing)
        /// </code>
        /// </summary>
        /// <param name="classMethod">Fully-qualified method in "Namespace.Class:Method" format.</param>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern bool IsMethodBaked(string classMethod);

        /// <summary>
        /// Returns the reason a typed hook on this method failed to bake (the same text logged at install), or null if it baked cleanly, has no typed hooks, or isn't hooked.
        /// Pre-flight a typed hook:
        /// <code>
        ///   string why = S3MM.Audit.GetBakeRejectReason("Foo.Bar:Baz");
        ///   if (why != null) S3MM.Log.Warn("Typed hook not baked: " + why);
        /// </code>
        /// Pair with <see cref="IsMethodBaked"/>: baked==false &amp;&amp; reason!=null tells you exactly why.
        /// A crash-quarantined target (2 native crashes attributed to the bake across runs) reports that here too; delete S3MM/baked_quarantine.txt to clear the strikes and re-bake, and check S3MM/baked_crashes.log for the per-crash forensics.
        /// </summary>
        /// <param name="classMethod">Fully-qualified method in "Namespace.Class:Method" format.</param>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string GetBakeRejectReason(string classMethod);

        /// <summary>
        /// Returns true if a core game assembly (ScriptCore, SimIFace, mscorlib) was replaced by a version loaded from a .package file.
        /// </summary>
        /// <param name="assemblyName">Short assembly name, e.g. "ScriptCore".</param>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern bool IsAssemblyReplaced(string assemblyName);

        /// <summary>Returns the total number of methods with at least one S3MM hook installed.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern int GetHookedMethodCount();

        /// <summary>Returns the total number of S3MM mods currently loaded.</summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern int GetLoadedModCount();

        // T2: Bulk queries, native builds typed arrays directly, WIP may change

        /// <summary>
        /// Returns all hook and watcher entries across all chains.
        /// Returns null if no hooks are installed.
        /// </summary>
        /// <example>
        /// HookInfo[] hooks = S3MM.Audit.GetAllHooks();
        /// // hooks[0].Target     = "Sims3.Gameplay.Actors.Sim:DoThing"
        /// // hooks[0].ModName    = "MyMod"
        /// // hooks[0].Priority   = 0
        /// // hooks[0].IsWatcher  = false
        /// </example>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern HookInfo[] GetAllHooks();

        /// <summary>
        /// Returns the hook chain for a single method, in dispatch order (watchers first, then hooks by priority).
        /// The first non-watcher entry is the chain entry point - its <see cref="HookInfo.IsEntry"/> is true.
        /// Returns null if the method has no hooks.
        /// </summary>
        /// <param name="classMethod">Fully-qualified method in "Namespace.Class:Method" format.</param>
        /// <example>
        /// HookInfo[] chain = S3MM.Audit.GetHookChain("Sims3.Gameplay.Actors.Sim:DoThing");
        /// // chain[0].Target     = "Sims3.Gameplay.Actors.Sim:DoThing"
        /// // chain[0].ModName    = "WatcherMod"
        /// // chain[0].Priority   = 1000000
        /// // chain[0].IsWatcher  = true
        /// // chain[0].IsEntry    = false
        /// // chain[1].ModName    = "HookMod"
        /// // chain[1].Priority   = 0
        /// // chain[1].IsWatcher  = false
        /// // chain[1].IsEntry    = true   <- first non-watcher entry holds the dispatch stub
        /// </example>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern HookInfo[] GetHookChain(string classMethod);

        /// <summary>
        /// Returns all replaced core assemblies with their source package info.
        /// Returns null if no assemblies were replaced.
        /// </summary>
        /// <example>
        /// ReplacedAssembly[] replaced = S3MM.Audit.GetReplacedAssemblies();
        /// // replaced[0].AssemblyName  = "ScriptCore"
        /// // replaced[0].SourcePackage = "MyOverhaul.package"
        /// </example>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern ReplacedAssembly[] GetReplacedAssemblies();

        /// <summary>
        /// Returns all loaded S3MM mod names.
        /// Returns null if no mods are loaded.
        /// </summary>
        /// <example>
        /// string[] mods = S3MM.Audit.GetLoadedMods();
        /// // mods[0] = "MyMod"
        /// // mods[1] = "AnotherMod"
        /// </example>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string[] GetLoadedMods();

        /// <summary>
        /// Returns "Class:Method - reason" for every hook of <paramref name="modName"/> that S3MM refused to install
        /// (signature/return-ABI mismatch, unhookable target, rejected typed entry), or null if none were refused.
        /// Hook.Register cannot report this itself (refusal is decided at chain install time, and a loosely-typed
        /// handler is legal at lower priority), so self-check at startup instead:
        /// <code>
        ///   string[] refused = S3MM.Audit.GetRefusedHooks("MyMod");
        ///   if (refused != null)
        ///       foreach (string r in refused) S3MM.Log.Error("Hook not installed: " + r);
        /// </code>
        /// The same refusals appear in the startup "Hook install summary" log block. Pass null/empty for all mods' refusals, prefixed "ModName: ".
        /// </summary>
        /// <param name="modName">Logical mod name (assembly name), or null/empty for all mods.</param>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern string[] GetRefusedHooks(string modName);
    }
}
