using System;
using System.Runtime.CompilerServices;

namespace S3MM
{
    /// <summary>
    /// Current S3MM API version. Incremented only when the managed API shape changes (added/removed/renamed ICALLs, changed signatures, attribute semantics).
    /// Internal native changes that don't affect the managed surface do not bump this.
    /// </summary>
    public static class Version
    {
        public const int Api = 1;
    }

    /// <summary>Native bridge for S3MM hook system. These methods call into the native S3MM code.</summary>
    public static class Native
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
        public static extern IntPtr FindClass(string assemblyName, string nameSpace, string className);

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
            if (result == null) return default(T);
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
        public static void Parse(string fullTarget, out string targetClass, out string targetMethod, out string signature)
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
    /// Call S3MM.Hook.CallNext() to invoke the next hook in the chain or the original.
    /// Code before CallNext = "prefix" behavior, code after = "postfix" behavior. Not calling CallNext effectively replaces the original method.
    /// </summary>
    /// <example>
    /// [S3MM.Hook("Sims3.Gameplay.Sim:DoThing(int,string)")]  // With signature for overloads
    /// [S3MM.Hook("Sims3.Gameplay.Sim:DoThing")]              // Any overload
    /// [S3MM.Hook("Sims3.Gameplay.Sim.DoThing")]              // Dot syntax also works
    /// </example>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    public class HookAttribute : Attribute
    {
        private string _targetClass;
        private string _targetMethod;
        private string _signature;  // Optional: "(int,string)" for overload resolution
        private int _priority;

        public string TargetClass { get { return _targetClass; } }
        public string TargetMethod { get { return _targetMethod; } }
        public string Signature { get { return _signature; } }
        public int Priority { get { return _priority; } set { _priority = value; } }

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
    /// </summary>
    /// <example>
    /// [S3MM.Watcher("Sims3.Gameplay.Sim:DoThing")]
    /// public static object WatchDoThing(Sim __instance) {
    ///     Logger.Log("DoThing was called!");
    ///     return Hook.CallNext(__instance);
    /// }
    /// </example>
    [AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
    public class WatcherAttribute : Attribute
    {
        private string _targetClass;
        private string _targetMethod;
        private string _signature;

        public string TargetClass { get { return _targetClass; } }
        public string TargetMethod { get { return _targetMethod; } }
        public string Signature { get { return _signature; } }

        /// <summary>Target format: "Namespace.Class:Method(args)" or "Namespace.Class.Method(args)". Signature is optional, for overload resolution.</summary>
        public WatcherAttribute(string fullTarget)
        {
            TargetParser.Parse(fullTarget, out _targetClass, out _targetMethod, out _signature);
        }
    }

    /// <summary>Helper class for hook methods. Provides convenience wrappers for chain traversal and original method invocation.</summary>
    // TODO: Could add a cross-hook state bag (Dictionary<string,object>) here if a real use case comes up, but local variables survive across CallNext since the managed frame stays alive, so I don't see the need tbh
    public static class Hook
    {
        /// <summary>
        /// Calls the next handler in the hook chain, or the original method if this is the last hook.
        /// Always resolves the token by walking interpreter frames for reliability.
        /// </summary>
        public static object CallNext(object instance, params object[] args)
        {
            uint token = Native.GetCallerHookToken();
            if (token == 0)
            {
                Log.Warn("CallNext: frame walk failed to resolve caller. Is this called from within a hook?");
                return null;
            }

            return Native.CallNext(token, instance, args);
        }

        /// <summary>
        /// Calls the true original method, skipping all hooks in the chain.
        /// Always resolves the token by walking interpreter frames for reliability.
        /// </summary>
        // TODO: Same as CallNext, verify native retry covers first-call bug fully.
        public static object CallOriginal(object instance, params object[] args)
        {
            uint token = Native.GetCallerHookToken();
            if (token == 0)
            {
                Log.Warn("CallOriginal: frame walk failed to resolve caller. Is this called from within a hook?");
                return null;
            }

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
        // byref my one true foe
        /// <summary>
        /// Wraps a value type for byref (ref/out) parameter passing to CallNext.
        /// When hooking methods with ref or out parameters, the original method needs to modify the caller's storage.
        /// Use this helper to create a single-element array that the native layer will pass by reference, after CallNext returns, read the modified value from index [0] and copy it back to your ref parameter.
        ///
        /// NOTE: The struct constraint is intentional. The native layer (UnboxParametersFast) treats
        /// ANY single-element array as a byref wrapper, so dropping the constraint would make it too easy to accidentally trigger byref semantics on a normal argument. 
        /// For reference-type ref params (e.g. ref uint[]), manually create a single-element array: new uint[1][] { idArray }.
        /// TODO: Could add a distinct byref marker type to disambiguate from real single-element arrays.
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
        public static T[] Ref<T>(T value) where T : struct
        {
            return new T[] { value };
        }
        // TODO: Resort
        /// <summary>
        /// Register a hook by raw method pointers. Use Cache.FindMethod() or MethodBase.MethodHandle.Value to obtain the IntPtr.
        /// hookType: 0 = Hook, 1 = Watcher.
        /// </summary>
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void RegisterByPointer(IntPtr targetMethod, IntPtr hookMethod, int hookType, int priority);

        /// <summary>
        /// Convenience: register a hook given MethodBase objects.
        /// Uses MethodHandle.Value to get the native MonoMethod* pointer.
        /// </summary>
        public static void Register(System.Reflection.MethodBase target, System.Reflection.MethodBase hook)
        {
            RegisterByPointer(target.MethodHandle.Value, hook.MethodHandle.Value, 0, 0);
        }

        /// <summary>
        /// Convenience: register a hook given MethodBase objects with priority.
        /// </summary>
        public static void Register(System.Reflection.MethodBase target, System.Reflection.MethodBase hook, int priority)
        {
            RegisterByPointer(target.MethodHandle.Value, hook.MethodHandle.Value, 0, priority);
        }

        /// <summary>
        /// Convenience: register a watcher given MethodBase objects.
        /// </summary>
        public static void RegisterWatcher(System.Reflection.MethodBase target, System.Reflection.MethodBase hook)
        {
            RegisterByPointer(target.MethodHandle.Value, hook.MethodHandle.Value, 1, 0);
        }

        /// <summary>
        /// Convenience: register a watcher given MethodBase objects with priority.
        /// </summary>
        public static void RegisterWatcher(System.Reflection.MethodBase target, System.Reflection.MethodBase hook, int priority)
        {
            RegisterByPointer(target.MethodHandle.Value, hook.MethodHandle.Value, 1, priority);
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
        /// Returns the hook chain for a single method.
        /// Returns null if the method has no hooks.
        /// </summary>
        /// <param name="classMethod">Fully-qualified method in "Namespace.Class:Method" format.</param>
        /// <example>
        /// HookInfo[] chain = S3MM.Audit.GetHookChain("Sims3.Gameplay.Actors.Sim:DoThing");
        /// // chain[0].Target     = "Sims3.Gameplay.Actors.Sim:DoThing"
        /// // chain[0].ModName    = "WatcherMod"
        /// // chain[0].Priority   = 1000000
        /// // chain[0].IsWatcher  = true
        /// // chain[1].ModName    = "HookMod"
        /// // chain[1].Priority   = 0
        /// // chain[1].IsWatcher  = false
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
    }
}
