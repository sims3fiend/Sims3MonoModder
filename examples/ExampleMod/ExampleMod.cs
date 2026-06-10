using System;
using System.Reflection;
using Sims3.SimIFace;
using Sims3.Gameplay.Actors;

// S3MM showcase / template mod. NOT a real gameplay mod
// Should hopefuly show most major parts of the S3MM API in one file so people can copy-paste/learn the API
// In-game: type `examplemod` at the cheat console (Ctrl+Shift+C) for a status dump, or `examplemod skip` to fire the skip-original demo on demand (gameplay never triggers it naturally)
// Right click the mod name and hot-reload to watch OnUnload restore state then OnLoad reinstall.
public static class ExampleMod
{
    // Cached native handles.
    // Looked up once in OnLoad, IntPtrs are stable for the process lifetime, so we never re-look-up.

    // Lot class + a couple of its static tunables (int + uint).
    private static IntPtr _lotClass;
    private static IntPtr _kInactiveSleepTicksField;  // static int
    private static IntPtr _kAutonomousVisitFreqField; // static float
    private static IntPtr _kFrontRoomQuadCountField;  // static uint

    // SimRoutingComponent - the routing component we hook for the instance + RTTI demo.
    private static IntPtr _simRoutingComponentClass;
    private static IntPtr _mSimWalkStyleField; // instance Sim.WalkStyle enum (read as int off __this)

    // AutonomyRestrictions - a known global gameplay switch (static bool).
    private static IntPtr _autonomyRestrictionsClass;
    private static IntPtr _sDisableAutonomyField;

    // RandomUtil.GetDouble(double,double) handle - cached so `examplemod skip` can drive the skip-original demo on demand.
    private static IntPtr _getDoubleMethod;

    // Stats the cheat dumps.
    private static int _coinFlipsObserved;
    private static int _getIntsHooked; // counted by HookGetInt (Priority 0)
    private static int _getIntsHookedEarly; // counted by HookGetIntEarly (Priority 100)
    private static int _updateWalkStyleHooks;
    private static int _callOriginalUses;
    private static int _skipOriginalUses;
    private static int _normalizeProgressHooks; // counted by HookNormalizeProgress (ref-param hook)

    // GC root for the cheat delegate - we must keep our own managed reference alive.
    // Sims 3's Mono marshals the delegate as a COM interface (a CCW), and Mono 1.2.3.1's CCW does not root its target.
    // Without this field the inline `new CommandHandler(...)` is collected on the next GC, and the next native invoke calls into freed memory and crashes.
    // (The game roots its handlers the same way, Commands.CommandRegistry keeps them in a static dictionary for the session)
    private static CommandHandler _cheatDelegate;

    // Original value of a static field we tweak in OnLoad - restored in OnUnload so hot-reload leaves the game clean.
    private static int _originalSleepTicks;
    private static bool _didTweakSleepTicks;

    [S3MM.OnLoad]
    public static void OnLoad()
    {
        S3MM.Log.Info("Loading (S3MM API v" + S3MM.Version.Api + ")");
        // --- Cache native handles via the Cache API: ---
        // FindClass(assembly, namespace, name)
        // Returns IntPtr.Zero on miss.
        _lotClass = S3MM.Cache.FindClass("Sims3GameplaySystems", "Sims3.Gameplay.Core", "Lot");
        if (_lotClass == IntPtr.Zero)
        {
            S3MM.Log.Warn("Could not find Sims3.Gameplay.Core.Lot, field reads disabled");
        }
        else
        {
            _kInactiveSleepTicksField  = S3MM.Cache.FindField(_lotClass, "kInactiveLotSleepTicks");
            _kAutonomousVisitFreqField = S3MM.Cache.FindField(_lotClass, "kAutonomousVisitFrequencyInDays");
            _kFrontRoomQuadCountField  = S3MM.Cache.FindField(_lotClass, "kFrontRoomTotalQuadrantCount");
            S3MM.Log.Debug("Lot handle = 0x" + _lotClass.ToInt32().ToString("X8"));
        }

        // SimRoutingComponent + its mSimWalkStyle field, read off __this in the UpdateWalkStyle hook below
        _simRoutingComponentClass = S3MM.Cache.FindClass(
            "Sims3GameplaySystems", "Sims3.Gameplay.Actors", "SimRoutingComponent");
        if (_simRoutingComponentClass != IntPtr.Zero)
        {
            _mSimWalkStyleField = S3MM.Cache.FindField(_simRoutingComponentClass, "mSimWalkStyle");
        }

        // AutonomyRestrictions.sDisableAutonomyForSelectedSim, public static bool.
        _autonomyRestrictionsClass = S3MM.Cache.FindClass(
            "Sims3GameplaySystems", "Sims3.Gameplay.Autonomy", "AutonomyRestrictions");
        if (_autonomyRestrictionsClass != IntPtr.Zero)
        {
            _sDisableAutonomyField = S3MM.Cache.FindField(
                _autonomyRestrictionsClass, "sDisableAutonomyForSelectedSim");
        }

        // --- Demonstrate typed static field read + safe mutation w/ restore ---
        // This shows the "modify game tunable, undo on unload" pattern.
        if (_kInactiveSleepTicksField != IntPtr.Zero)
        {
            _originalSleepTicks = S3MM.Cache.GetStaticFieldInt32(_lotClass, _kInactiveSleepTicksField);
            // Sentinel tweak - write back the same value, just to prove the call works.
            // Real mods would write a new value here.
            S3MM.Cache.SetStaticFieldInt32(_lotClass, _kInactiveSleepTicksField, _originalSleepTicks);
            _didTweakSleepTicks = true;
            S3MM.Log.Info("kInactiveLotSleepTicks = " + _originalSleepTicks + " (cached for restore)");
        }

        // --- Demonstrate typed reads of more static field shapes (no mutation). ---
        // uint reader:
        if (_kFrontRoomQuadCountField != IntPtr.Zero)
        {
            uint quads = S3MM.Cache.GetStaticFieldUInt32(_lotClass, _kFrontRoomQuadCountField);
            S3MM.Log.Info("Lot.kFrontRoomTotalQuadrantCount (uint) = " + quads);
        }
        // bool reader:
        if (_sDisableAutonomyField != IntPtr.Zero)
        {
            bool v = S3MM.Cache.GetStaticFieldBool(_autonomyRestrictionsClass, _sDisableAutonomyField);
            S3MM.Log.Info("AutonomyRestrictions.sDisableAutonomyForSelectedSim (bool) = " + v);
        }

        // Register a console cheat.
        // Sims3 CommandSystem expects an int-returning CommandHandler delegate; the name is registered bare (same as built-in cheats like "freeRealEstate") and typed as-is at the console.
        _cheatDelegate = new CommandHandler(CheatHandler);
        bool ok = CommandSystem.RegisterCommand(
            "examplemod",
            "Dumps ExampleMod status to the log. `examplemod skip` runs the skip-original demo.",
            _cheatDelegate);
        if (ok) S3MM.Log.Info("Registered cheat examplemod");
        else    S3MM.Log.Error("Failed to register cheat examplemod");

        // Dynamic hook registration (no attribute) - same effect as [S3MM.Hook] but resolved at runtime via reflection.
        // Useful when the target method is only known at load time (e.g. plugin discovery, conditional patches, etc.).
        try
        {
            MethodInfo hookMi = typeof(ExampleMod).GetMethod(
                "DynamicGetFloatHook", BindingFlags.NonPublic | BindingFlags.Static);
            IntPtr randomUtilClass = S3MM.Cache.FindClass(
                "Sims3GameplaySystems", "Sims3.Gameplay.Core", "RandomUtil");
            IntPtr getFloat = randomUtilClass != IntPtr.Zero
                ? S3MM.Cache.FindMethod(randomUtilClass, "GetFloat", 2)
                : IntPtr.Zero;
            if (getFloat != IntPtr.Zero && hookMi != null)
            {
                S3MM.Hook.RegisterByPointer(getFloat, hookMi.MethodHandle.Value, 0, 0);
                S3MM.Log.Debug("Dynamically hooked RandomUtil.GetFloat(float,float)");
            }
        }
        catch (Exception ex)
        {
            S3MM.Log.Error("Dynamic hook registration failed: " + ex.Message);
        }

        // --- Demo Cache.Invoke - resolve a MonoMethod* and call the game directly ---
        // Invoke boxes value-type args and the return for you; GetMethodName confirms what we resolved ("Namespace.Class::Method").
        // SelectOneRandomBit(uint)->uint picks one set bit from a mask and internally calls our hooked GetInt, so this also proves Invoke runs the REAL method through the live hook chain.
        IntPtr randomUtilForInvoke = S3MM.Cache.FindClass("Sims3GameplaySystems", "Sims3.Gameplay.Core", "RandomUtil");
        // Cache GetDouble(double,double) for the `examplemod skip` cheat (drives the skip-original demo via Cache.Invoke).
        _getDoubleMethod = randomUtilForInvoke != IntPtr.Zero
            ? S3MM.Cache.FindMethod(randomUtilForInvoke, "GetDouble", 2)
            : IntPtr.Zero;
        IntPtr selectBit = randomUtilForInvoke != IntPtr.Zero
            ? S3MM.Cache.FindMethod(randomUtilForInvoke, "SelectOneRandomBit", 1)
            : IntPtr.Zero;
        if (selectBit != IntPtr.Zero)
        {
            S3MM.Log.Debug("Resolved method handle: " + S3MM.Cache.GetMethodName(selectBit));
            uint pick = S3MM.Cache.Invoke<uint>(selectBit, null, new object[] { (uint)0xA }); // mask 0b1010 -> one of 0x2 / 0x8
            S3MM.Log.Info("Invoke SelectOneRandomBit(0xA) -> 0x" + pick.ToString("X"));
        }

        S3MM.Log.Info("Load complete - " + S3MM.Audit.GetLoadedModCount() + " mods loaded, "
            + S3MM.Audit.GetHookedMethodCount() + " methods hooked globally");
    }

    [S3MM.OnUnload]
    public static void OnUnload()
    {
        // Restore anything we changed BEFORE hooks are torn down/removed
        if (_didTweakSleepTicks && _kInactiveSleepTicksField != IntPtr.Zero)
        {
            S3MM.Cache.SetStaticFieldInt32(_lotClass, _kInactiveSleepTicksField, _originalSleepTicks);
            _didTweakSleepTicks = false;
        }

        // Console cheats are NOT unregistered automatically obvi
        CommandSystem.UnregisterCommand("examplemod");

        S3MM.Log.Info("Unloaded - observed " + _coinFlipsObserved + " coin flips, "
            + _getIntsHooked + " GetInt calls (P0), "
            + _getIntsHookedEarly + " GetInt calls (P100), "
            + _updateWalkStyleHooks + " UpdateWalkStyle calls, "
            + _callOriginalUses + " CallOriginal invocations, "
            + _skipOriginalUses + " skip-original short-circuits, "
            + _normalizeProgressHooks + " NormalizeProgress ref-hooks");
    }

    // Hook on a static method with value-type args.
    // Code before CallNext is the "prefix" (runs before the original), code after is the "postfix" (runs after).
    // Not calling CallNext at all skips the original entirely - see HookGetDoubleSkip.
    [S3MM.Hook("Sims3.Gameplay.Core.RandomUtil:GetInt(int,int)")]
    public static int HookGetInt(int min, int max)
    {
        // prefix: count + (optionally) inspect args
        _getIntsHooked++;

        // run the rest of the chain (other mods' hooks, then the original)
        int result = S3MM.Hook.CallNext<int>(null, min, max);

        // postfix: log occasionally.
        // Don't spam - this method is hot.
        if ((_getIntsHooked & 0xFF) == 0)
            S3MM.Log.Debug("GetInt(" + min + "," + max + ") -> " + result + " (call #" + _getIntsHooked + ")");

        return result;
    }

    // Same target as HookGetInt but Priority = 100, so it runs FIRST.
    // Higher Priority = earlier in the chain (default 0); watchers always precede all hooks (Priority + 1000000).
    // The examplemod audit dump lists this entry before HookGetInt (P0), then the original.
    [S3MM.Hook("Sims3.Gameplay.Core.RandomUtil:GetInt(int,int)", Priority = 100)]
    public static int HookGetIntEarly(int min, int max)
    {
        _getIntsHookedEarly++;
        // Pass through unchanged - we're just here to demonstrate ordering.
        return S3MM.Hook.CallNext<int>(null, min, max);
    }

    // Hook on an INSTANCE method
    // Instance methods pass the receiver as arg0, so the hook must take it as the FIRST param, AKA here `__this`
    // Type it as any ref type compatible with the receiver; `object` keeps us off a hard dep on the target's assembly
    // CallNext mirrors this: pass the instance as its first arg, NOT null like static hooks do
    // UpdateWalkStyle re-resolves a Sim's walk style during routing, so this fires whenever that happens
    // Also shows a typed instance field read (GetFieldInt32) off __this and reflection-free RTTI on the instance
    [S3MM.Hook("Sims3.Gameplay.Actors.SimRoutingComponent:UpdateWalkStyle")]
    public static void HookUpdateWalkStyle(object __this)
    {
        _updateWalkStyleHooks++;

        S3MM.Hook.CallNext(__this);   // void method, instance, pass __this (NOT null); ignore the (null) return

        // Sample-log occasionally so we can see the instance binding work without spamming the routing path.
        if ((_updateWalkStyleHooks & 0x3F) == 0)
        {
            // Typed instance field read: mSimWalkStyle is a Sim.WalkStyle enum, read straight as int off __this (enums are int-backed, no boxing).
            int walkStyleRaw = _mSimWalkStyleField != IntPtr.Zero ? S3MM.Cache.GetFieldInt32(__this, _mSimWalkStyleField) : -1;
            // Cast the raw int to the enum for a readable name (e.g. "AutoSelect" / "Walk" / "Run"); ToString falls back to the number for any undefined value.
            string walkStyle = ((Sim.WalkStyle)walkStyleRaw).ToString();

            // Cache.IsEnum demo (reflection-free): box the value to get its MonoClass* via GetObjectClass, then ask the native layer whether that class is an enum - no managed System.Type involved.
            IntPtr walkStyleClass = S3MM.Cache.GetObjectClass((Sim.WalkStyle)walkStyleRaw);
            bool walkStyleIsEnum = walkStyleClass != IntPtr.Zero && S3MM.Cache.IsEnum(walkStyleClass);

            // Reflection-free RTTI: ask the native layer what __this actually is at runtime.
            // GetObjectClass returns the MonoClass*, then GetClassNamespace/GetClassName/GetParentClass walk it - no managed System.Type involved.
            IntPtr klass = S3MM.Cache.GetObjectClass(__this);
            IntPtr baseKlass = S3MM.Cache.GetParentClass(klass);
            string runtimeType = S3MM.Cache.GetClassNamespace(klass) + "." + S3MM.Cache.GetClassName(klass);
            string baseType = baseKlass != IntPtr.Zero ? S3MM.Cache.GetClassName(baseKlass) : "(none)";

            S3MM.Log.Debug("SimRoutingComponent.UpdateWalkStyle (mSimWalkStyle=" + walkStyle
                + " [isEnum=" + walkStyleIsEnum + "], __this=" + runtimeType + " : " + baseType
                + ", call #" + _updateWalkStyleHooks + ")");
        }
    }

    // CallOriginal - bypass the rest of the chain and invoke the unpatched original directly.
    // Use when you explicitly need unmodified behavior (e.g. defensive math other mods might be skewing) or want to compare original-vs-chained results.
    // This SKIPS every hook below this one in priority order, don't use it for normal pass-through, that's what CallNext is for.
    [S3MM.Hook("Sims3.Gameplay.Core.RandomUtil:RandomChance(float)", Priority = 50)]
    public static bool HookRandomChanceOriginal(float chance)
    {
        _callOriginalUses++;
        // The branch below is an arbitrary demo split, it just lets us show both dispatch primitives in one method.
        // CallNext: continue down the chain (other mods' hooks, then the original).
        if (chance < 0f || chance > 100f)
            return S3MM.Hook.CallNext<bool>(null, chance);

        // CallOriginal: skip the rest of the chain, hit the unpatched original directly.
        return S3MM.Hook.CallOriginal<bool>(null, chance);
    }

    // Skip-original - return WITHOUT calling CallNext, so the original and all lower-priority hooks NEVER run on this call.
    // Guarded behind a sentinel so the demo doesn't break the game: GetDouble(min,max) with min==max is degenerate (the game's own impl returns min anyway)
    // Gameplay never calls GetDouble with min==max, so this never fires during normal play - run `examplemod skip` at the console to drive it via Cache.Invoke and watch the counter move.
    // Real mods should use this for "always succeed", "block this method entirely", etc.
    [S3MM.Hook("Sims3.Gameplay.Core.RandomUtil:GetDouble(double,double)")]
    public static double HookGetDoubleSkip(double minValue, double maxValue)
    {
        if (minValue == maxValue)
        {
            _skipOriginalUses++;
            // Notice: no CallNext, no CallOriginal - the chain stops here.
            return minValue;
        }
        // Everything else: pass through normally.
        return S3MM.Hook.CallNext<double>(null, minValue, maxValue);
    }

    // Hook on a method with a ref/out param - the trickiest case, handled by Hook.Ref<T> byref marshalling.
    // SkillMeter.NormalizeProgress(ref float, float) clamps currentProgress to [0,1] then scales it, so the original WRITES through the ref
    // We must copy the result back or the caller sees stale data.
    // Hook.Ref<float> wraps the value in a single-element float[] that the native layer recognizes as a byref cell; after CallNext we read index [0] back into our ref param.
    // Signature uses `float&` to denote the byref param (matches the IL `Single&`); the trailing plain `float` is the by-value maxFluidHeight.
    [S3MM.Hook("Sims3.Gameplay.Core.SkillMeter:NormalizeProgress(float&,float)")]
    public static void HookNormalizeProgress(ref float currentProgress, float maxFluidHeight)
    {
        _normalizeProgressHooks++;
        float before = currentProgress;

        float[] progressRef = S3MM.Hook.Ref(currentProgress); // wrap for byref pass-through
        S3MM.Hook.CallNext(null, progressRef, maxFluidHeight); // void return - non-generic CallNext, ignore the (null) result
        currentProgress = progressRef[0]; // copy the original's byref write back into our ref param

        if ((_normalizeProgressHooks & 0x3F) == 0)
            S3MM.Log.Debug("NormalizeProgress(" + before + ", " + maxFluidHeight + ") -> " + currentProgress
                + " (call #" + _normalizeProgressHooks + ")");
    }

    // Watcher - read-only observation, runs before all hooks
    // Always calls CallNext and returns its result unchanged
    // Watchers are for logging/metrics/debugging; shouldn't be used to modify args, side-effects, or the return value
    [S3MM.Watcher("Sims3.Gameplay.Core.RandomUtil:CoinFlip")]
    public static bool WatchCoinFlip()
    {
        _coinFlipsObserved++;
        return S3MM.Hook.CallNext<bool>(null);
    }

    // Dynamically-registered hook target
    // Wired up in OnLoad via S3MM.Hook.RegisterByPointer, no [S3MM.Hook] attribute needed
    private static float DynamicGetFloatHook(float min, float max)
    {
        float result = S3MM.Hook.CallNext<float>(null, min, max);
        // (intentionally silent - just demonstrating the registration path)
        return result;
    }

    // Cheat subcommand: `examplemod skip` deterministically fires the skip-original path
    // RandomUtil.GetDouble(min,max) with min==max is a degenerate range, the game's own impl returns min gameplay never calls it that way, so we drive it here via Cache.Invoke to prove the path works.
    private static int CheatSkipDemo()
    {
        S3MM.Log.Info("======== examplemod skip demo ========");
        if (_getDoubleMethod == IntPtr.Zero)
        {
            S3MM.Log.Error("GetDouble(double,double) handle not resolved - cannot run skip demo");
            return 0;
        }

        int before = _skipOriginalUses;
        // Invoke runs the REAL method through the live hook chain, so this reaches HookGetDoubleSkip, which sees min==max and returns minValue WITHOUT calling the original.
        double result = S3MM.Cache.Invoke<double>(_getDoubleMethod, null, new object[] { 5.0, 5.0 });
        S3MM.Log.Info("Invoke GetDouble(5,5) -> " + result + "  (skip-original count " + before + " -> " + _skipOriginalUses + ")");

        if (_skipOriginalUses > before)
            S3MM.Log.Info("Skip-original fired: the original RandomUtil.GetDouble was NOT executed.");
        else
            S3MM.Log.Warn("Skip-original did NOT fire - is the GetDouble hook installed?");

        S3MM.Log.Info("======================================");
        return 1;
    }

    // The examplemod cheat dumps a snapshot of S3MM state to the S3MM log (the in-game ImGui log window and log file).
    // `examplemod skip` runs the skip-original demo instead (see CheatSkipDemo)
    private static int CheatHandler(object[] parameters)
    {
        // Subcommand dispatch.
        // The dispatcher strips the command name before calling us, so parameters[0] is the FIRST ARG h-haha...
        // Bare `examplemod` (zero args) falls through to the status dump.
        string sub = (parameters != null && parameters.Length > 0) ? ("" + parameters[0]).Trim().ToLower() : "";
        if (sub == "skip")
            return CheatSkipDemo();

        S3MM.Log.Info("======== examplemod status ========");
        S3MM.Log.Info("S3MM API version       : " + S3MM.Version.Api);
        S3MM.Log.Info("Loaded mods            : " + S3MM.Audit.GetLoadedModCount());
        S3MM.Log.Info("Hooked methods         : " + S3MM.Audit.GetHookedMethodCount());
        S3MM.Log.Info("CoinFlips observed     : " + _coinFlipsObserved);
        S3MM.Log.Info("GetInt calls (P0)      : " + _getIntsHooked);
        S3MM.Log.Info("GetInt calls (P100)    : " + _getIntsHookedEarly);
        S3MM.Log.Info("UpdateWalkStyle hooks  : " + _updateWalkStyleHooks);
        S3MM.Log.Info("CallOriginal uses      : " + _callOriginalUses);
        S3MM.Log.Info("Skip-original uses     : " + _skipOriginalUses);
        S3MM.Log.Info("NormalizeProgress hooks: " + _normalizeProgressHooks);

        // List every loaded mod by name.
        string[] mods = S3MM.Audit.GetLoadedMods();
        if (mods != null)
        {
            S3MM.Log.Info("-- Loaded mods --");
            for (int i = 0; i < mods.Length; i++)
                S3MM.Log.Info("  [" + i + "] " + mods[i]);
        }

        // Show any replaced core assemblies (ScriptCore etc).
        S3MM.ReplacedAssembly[] replaced = S3MM.Audit.GetReplacedAssemblies();
        if (replaced != null && replaced.Length > 0)
        {
            S3MM.Log.Info("-- Replaced core assemblies --");
            for (int i = 0; i < replaced.Length; i++)
                S3MM.Log.Warn("  " + replaced[i].AssemblyName + " <- " + replaced[i].SourcePackage);
        }

        // Show the hook chain for one specific method to illustrate GetHookChain
        // GetInt:int,int has two of our hooks attached, so this proves priority orderin
        S3MM.HookInfo[] chain = S3MM.Audit.GetHookChain("Sims3.Gameplay.Core.RandomUtil:GetInt");
        if (chain != null)
        {
            S3MM.Log.Info("-- Chain on RandomUtil:GetInt (highest priority first) --");
            for (int i = 0; i < chain.Length; i++)
            {
                S3MM.Log.Info("  " + chain[i].ModName
                    + " (priority=" + chain[i].Priority
                    + ", watcher=" + chain[i].IsWatcher + ")");
            }
        }

        // Defensive-audit self-check, "is anyone (including us) patching this / replacing core assemblies?"
        // IsMethodHooked is a fast bool ICALL (safe for hot paths like per-frame guards); GetAllHooks returns the whole table.
        S3MM.Log.Info("-- Audit self-check --");
        S3MM.Log.Info("  IsMethodHooked(RandomUtil:GetInt)    = "
            + S3MM.Audit.IsMethodHooked("Sims3.Gameplay.Core.RandomUtil:GetInt"));
        S3MM.Log.Info("  IsMethodHooked(RandomUtil:GetDouble) = "
            + S3MM.Audit.IsMethodHooked("Sims3.Gameplay.Core.RandomUtil:GetDouble"));
        S3MM.Log.Info("  IsAssemblyReplaced(ScriptCore)       = "
            + S3MM.Audit.IsAssemblyReplaced("ScriptCore"));
        S3MM.HookInfo[] allHooks = S3MM.Audit.GetAllHooks();
        S3MM.Log.Info("  GetAllHooks() total entries          = "
            + (allHooks != null ? allHooks.Length : 0));

        // Dump cached static tunables across multiple typed accessors.
        if (_lotClass != IntPtr.Zero)
        {
            S3MM.Log.Info("-- Cached Lot tunables --");
            if (_kInactiveSleepTicksField != IntPtr.Zero)
                S3MM.Log.Info("  kInactiveLotSleepTicks (int)         = "
                    + S3MM.Cache.GetStaticFieldInt32(_lotClass, _kInactiveSleepTicksField));
            if (_kAutonomousVisitFreqField != IntPtr.Zero)
                S3MM.Log.Info("  kAutonomousVisitFrequencyInDays (f)  = "
                    + S3MM.Cache.GetStaticFieldFloat(_lotClass, _kAutonomousVisitFreqField));
            if (_kFrontRoomQuadCountField != IntPtr.Zero)
                S3MM.Log.Info("  kFrontRoomTotalQuadrantCount (uint)  = "
                    + S3MM.Cache.GetStaticFieldUInt32(_lotClass, _kFrontRoomQuadCountField));
        }
        if (_sDisableAutonomyField != IntPtr.Zero)
        {
            S3MM.Log.Info("-- Cached AutonomyRestrictions --");
            S3MM.Log.Info("  sDisableAutonomyForSelectedSim (bool) = "
                + S3MM.Cache.GetStaticFieldBool(_autonomyRestrictionsClass, _sDisableAutonomyField));
        }

        S3MM.Log.Info("====================================");
        return 1; // CommandHandler convention: non-zero = success
    }
}
