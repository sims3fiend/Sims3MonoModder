# S3MM Examples

Worked example mods for [Sims3MonoModder](../README.md). Each folder is a self-contained mod you can copy into your S3MM source directory and load and tweak.

To run one, copy its folder into:

```
Documents/Electronic Arts/The Sims 3/S3MM/Source/<ModName>/
```

then launch the game (or hit **Scan for New Mods** in the Mods panel). S3MM compiles the `.cs` on the fly against the game's assemblies + `S3MM.Helper`.

The API these target lives in [`S3MM.Helper/S3MM.cs`](../S3MM.Helper/S3MM.cs).

> This will be expanded to separate mod per domain/concept eventually... hopefully. Actual mods will get their own repo.

## Tips

> Catch-all for modding gotchas for now. This will move to a proper wiki once one exists.

### UI windows

- **Modifiers are postfix** `.Tooltip`, `.Hover`, `.Color`, `.Style` and `BeginContextMenu()` bind to the last *item* widget emitted before them. Layout ops (`SameLine`, `Separator`, `Spacing`, `Indent`/`Unindent`, `ItemWidth`, `BeginDisabled`/`EndDisabled`) emit no item, so they can sit between a widget and its modifiers.
- **`Style` / `Color` only apply to `Label`** On anything else they log a warning and do nothing, with no compile-time check.
- **`Hover` is edge-triggered and opt-in**
- **Slider `onChange` fires on release** (or on a committed Ctrl+click value), not per frame.
- **The title-bar X hides, it doesn't close** It fires `OnHidden`, and the window keeps its widgets, handlers and state until `Show()` brings it back. `Close()` drops the GC root on the window and its handlers, and `Show()` won't revive it. `OnClosed` also fires when S3MM force-closes leftover windows on hot-reload, Call `Close()` in `[OnUnload]`.

### `Chain.CallNext` vs `Hook.CallNext`: which one?

Prefer the typed `S3MM.Chain.CallNext<R, T0, ...>`, first type arg is the return type, the rest are the param types. Baking is automatic: match the target's signature exactly and the chain bakes to zero-overhead native dispatch. If it can't bake (a boxing hook shares the target, a signature mismatch), a by-value typed call transparently falls back to boxing dispatch, so you lose nothing by trying, and should generally try.

`S3MM.Hook.CallNext` is the boxing catch-all. Use it when the handler's signature deliberately doesn't match the target (loosely-typed `object self` catch-alls, dynamically-registered handlers, inflated generic targets), or when the target has more than 8 parameters, the typed generics cap at 8 forwarded params.

### Delegate lifetime

When you pass a delegate to a Sims 3 native API that marshals it as a COM CCW, e.g. `Sims3.SimIFace.CommandSystem.RegisterCommand`, or anything declared `[MarshalAs(UnmanagedType.Interface)]`; you **must** keep that delegate alive with a managed root on your side.

The CCW in Mono 1.2.3.1 does **not** keep the target delegate rooted. So an inline `new CommandHandler(MyThing)` gets collected on the next GC, and the next native invoke will crash with:

```
* Assertion at object.c:2150, condition `im' not met
```

The fix is to store the delegate in a `static` (or instance) field for as long as you need it.

```csharp
private static CommandHandler _cheatDelegate;
// ...
_cheatDelegate = new CommandHandler(MyHandler);
CommandSystem.RegisterCommand("foo", "desc", _cheatDelegate);
```

### `ref` / `out` parameters: `Chain.ByRef` (typed) or `Hook.Ref` (boxing)
On the boxing path, wrap the value with `Hook.Ref(value)` to pass it byref through `CallNext`, then read the modified value back from index `[0]` afterward.

```csharp
var valueRef = Hook.Ref(value);
bool ok = Hook.CallNext<bool>(self, key, valueRef);
value = valueRef[0];
```

On the typed path, `Chain.ByRef(ref value)` smuggles the pointer straight into the baked call, true ref/out forwarding, no array round-trip, no copy-back:

```csharp
[S3MM.Hook("Some.Class:TryGetValue(int,int&)")]
public static bool HookTryGetValue(object self, int key, ref int value)
{
    return S3MM.Chain.CallNext<bool, object, int, int>(self, key, S3MM.Chain.ByRef(ref value));
}
```

One hard req/rule: `Chain.ByRef` only works **baked**, it's a marker the bake rewrite nops out, and an `object[]` can't carry a managed pointer. A hook using it can't fall back to boxing, so if it can't bake it is **rejected at install** rather than demoted, check the log or `Audit.GetBakeRejectReason` for why. Compare `RefDoubleTypedHook` vs `HookNormalizeProgress` in ExampleMod for the two routes side by side. I may improve this in the future

### Baked-hook crash quarantine (`baked_quarantine.txt`)
Typed `Chain.CallNext` hooks bake to zero-overhead native dispatch. If a baked hook crashes the game natively (an access violation inside the interpreter, not a managed exception), S3MM attributes the crash to the baked target and records a strike in `S3MM/baked_quarantine.txt`. After **2 strikes across runs**, that target refuses to bake on launch and its typed hooks are demoted to the boxing dispatch path , your mod keeps working, just without the baked fast path, so a bad bake can't crash-loop the game. This is a little jank but the only sollution I could really think of.

If your hook stops baking, check:

- `S3MM.Audit.GetBakeRejectReason("Class:Method")`: returns the quarantine reason (the same text logged at install).
- `S3MM/baked_crashes.log`: a per-crash record (faulting link, `this`, argument values) for debugging the handler.

Once you've fixed the handler (or if the crashes were unrelated flukes), **delete `baked_quarantine.txt`** to clear the strikes and let the target bake again. One strike alone does nothing, a single transient crash won't disable a good bake.

### Declare the target's EXACT return type, never `object`
A hook's declared C# return type **is** the return ABI; there is no marshalling layer between your hook and the game's callers. Declaring `object` on a target that returns an enum or struct hands every caller a boxed-object *pointer* where it expected the value, autonomy sees venue type `0x41234569` instead of e.g. `WerewolfBar` and quietly breaks. Reference-type returns (`string`, classes) are unaffected.

S3MM refuses to install a shape-mismatched **entry** hook (the highest-priority hook on a target) with an ERROR naming the exact type to declare, so the mismatch fails loud at install instead of corrupting at runtime. Loosely-typed catch-all handlers are still fine at lower priority behind a shape-matched entry hook.

Refusals are also rolled up per mod in the startup **hook install summary** (`MyMod: 5 hooks requested, 4 installed, 1 REFUSED (declared 'object', target returns 'bool')`, look in the log for `Hook install summary`), and via `S3MM.Audit.GetRefusedHooks("MyMod")`, which returns a `"Class:Method - reason"` string per refused hook so a mod can self-check and complain in its own log instead of silently reverting.

Value-type returns of **any width** work when declared exactly, the runtime pre-boxes the return so even a 16-byte `ResourceKey` round-trips intact e.g.:

```csharp
[S3MM.Hook("Sims3.Gameplay.MapTags.MapTagsModel:GetVenueIconKey")]
public static ResourceKey HookVenueIconKey(object self, Sims3.UI.IMapTag tag)
{
    return S3MM.Chain.CallNext<ResourceKey, object, Sims3.UI.IMapTag>(self, tag);
}
```

See `HookVenueType` / `HookVenueIconKey` in ExampleMod; they hook the two methods from the bug report that motivated the rule.

The rule applies to `[S3MM.Watcher]` methods unchanged. A watcher sits in the return path like any other hook, so "read-only" describes your intent, not an exemption from the return contract. Declare the target's exact return type and pass the value through untouched.

### Rentry hooks can silently shadow every hook below them

A hook that returns **without calling `CallNext`** short-circuits the chain: no lower-priority hook and not even the original runs on that call. That is the point of skip-original, but it cuts both ways, if the short-circuiting hook isn't yours, YOUR hook "never fires" even though the install, the audit, and `IsMethodHooked` all will say it's hooked.

The dispatch order is watchers first, then hooks by priority, and the first non-watcher hook is the **entry** (its body is what callers dispatch into). Hooks at EQUAL priority keep registration order, which is mod load order, so two mods doing the same trick on the same target collide with no warning.

When a hook isn't firing, dump the chain before suspecting the install:

```csharp
S3MM.HookInfo[] chain = S3MM.Audit.GetHookChain("Some.Class:Method");
// One entry per handler, in dispatch order. chain[0] has IsEntry == true (a watcher if the target has one) if that's another mod and it short-circuits, then your hook is starved, not broken.
```

If you short-circuit deliberately, either pick a priority that makes the shadowing intentional, or gate it so normal chains pass through. I may add additional logging/stuff for this in the future.

For dispatch-level debugging, set `LogHookInvocations = true` in `S3MM/S3MM.toml`: every chain entry, `CallNext` step (with the invoking mod), and return-boxing decision gets logged.

### Hooking methods on generic / nested-in-generic types

Methods reached through a generic type, including the very common interaction shape `Sim.GoToVirtualHome.Definition.Test`, where the nested `Definition` inherits everything from `InteractionDefinition<TActor,TTarget,TInteraction>`, resolve via reflection to *inflated* methods on a generic instantiation. These are fully hookable:

- The attribute parser can't name nested/generic targets, so register with `MethodHandle.Value` + `Hook.RegisterByPointer` (see the generic-target demo in ExampleMod's `OnLoad`).
- Inflated targets always run on the boxing dispatch path (typed baking is refused for them), so forward with the boxing `S3MM.Hook.CallNext`.
- **Open** generic definitions (`typeof(Foo<>)` shapes, `ContainsGenericParameters == true`) are refused at registration with a logged ERROR; hook a constructed instantiation instead.

ExampleMod's demo demo's this with `examplemod generic` in the cheat console.

### Registering hooks after load needs `Hook.ApplyPending`

Registrations made during `OnLoad` are installed automatically once load finishes. If you register later at runtime (conditional patches, deferred discovery), nothing happens until you call `S3MM.Hook.ApplyPending()`: it installs new chains and reinstalls changed ones in place, no mod reload needed.

You also rarely need to spell `RegisterByPointer(target, hook, 0, 0)` yourself. The `Hook.Register(target, hook[, priority])` / `Hook.RegisterWatcher(...)` overloads take `MethodBase` or `IntPtr` targets in any mix and fill in the raw hookType int for you.

### Cache handles; use the typed accessors on hot paths

`FindClass` / `FindField` / `FindMethod` return native Mono pointers that are stable for the lifetime of the process, so resolve them once (in `OnLoad`) and stash them in `static` fields rather than looking them up every call. On per-frame paths, prefer the typed `GetFieldInt32` / `SetFieldFloat` / etc. over `GetField` / `SetField` to skip boxing.

For event-ish (non-hot) code there are by-name conveniences that skip the handle bookkeeping entirely: `Cache.GetField(obj, "mAge")`, `Cache.SetStaticField(klass, "kFoo", value)`, `Cache.Call(obj, "DoThing", args)` / `Cache.CallStatic(klass, "DoThing", args)`. Two behaviors to know:

- A missing member **throws** `ArgumentException`, so a typo'd name is loud at the call site. A `null` from `GetField` always means the field's value is null, never "field not found".
- Method resolution is by name + **argument count** only. For overloads that differ only by parameter types, resolve the handle explicitly with `FindMethod` and use `Invoke`.

### Who may I ask is calling? `Hook.CalledFrom` / `Hook.CallerInstance` / `Hook.GetCallerArgs`

From inside a handler you can ask about the game method that called the hooked target. Each member walks the interpreter frames on demand (nothing is captured per call), so the cost scales with what you actually read/use:

- `Hook.CalledFrom(IntPtr)` / `Hook.CalledFrom("Namespace.Class::Method")` and `Hook.CallerMethod` are allocation-free. Gate a hook on its call site with these; cache the `IntPtr` from `Cache.FindMethod` as the cheapest option.
- `Hook.CallerInstance` is the caller's `this` (null for statics), `Hook.CallerName` allocates the name string.
- `Hook.GetCallerArgs()` boxes a copy of every parameter of the caller. Don't use on something frequently used or.

You get the caller of the *innermost* hooked method below you, so a target called from another hook's handler reports that handler. Native/icall/whatever/engine callers (simulator tasks, event dispatch, `BeginInvoke` targets) have no managed frame and report `IntPtr.Zero` / `null` / `false`. Values are a view of the caller's frame, not a snapshot.

### Use `[S3MM.OnLoad]`, not static constructors

An exception in a static constructor becomes a permanent `TypeInitializationException`; the type is dead for the rest of the session. `[S3MM.OnLoad]` runs after your hooks are registered and applied, exceptions are caught and logged with your mod's name attribution, multiple OnLoad methods are supported, and it re-fires after hot-reload. Pair it with `[S3MM.OnUnload]` (runs before hooks are torn down) to restore any game state you changed; see the sleep-ticks save/restore pattern in ExampleMod.

### Persisting data in the save: `[PersistableStatic]`

Mark the assembly (`[assembly: PersistableStatic]` at the top of a source file) and each static field you want saved with `[PersistableStatic]`; the field is written into the save game and restored on load. This is what valinna mods use and the save format is identical, so data should move if a mod migrates. See `_cheatUsesEver` in ExampleMod.

- Game/mscorlib types (`int`, `string`, `Dictionary<K,V>`, ...) work. A type defined in your own mod persists too if you mark it `[Persistable]`.
- On hot-reload S3MM re-registers the fields on the new image and carries current values across , except values whose type is defined in the mod itself (they reset to default so stage them in `[S3MM.OnUnload]`/`[S3MM.OnLoad]` if you need them to survive).
- `[PersistableStatic(true)]` (`LoadFromMainMenuOnly`) should be honored the same way vanilla honors it.
- A save written before your mod existed loads fine (missing entries are simply absent), and removing the mod later doesn't break the save. I may migrate this entire system to a more S3MM controlled one or do some sort of bigger save-touching thing in the future, but this will likely remain in the API regardless.