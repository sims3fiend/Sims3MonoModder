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

### `ref` / `out` parameters need `Hook.Ref`

Wrap the value with `Hook.Ref(value)` to pass it byref through `CallNext`, then read the modified value back from index `[0]` afterward.

```csharp
var valueRef = Hook.Ref(value);
bool ok = Hook.CallNext<bool>(self, key, valueRef);
value = valueRef[0];
```

### Cache handles; use the typed accessors on hot paths

`FindClass` / `FindField` / `FindMethod` return native Mono pointers that are stable for the lifetime of the process, so resolve them once (in `OnLoad`) and stash them in `static` fields rather than looking them up every call. On per-frame paths, prefer the typed `GetFieldInt32` / `SetFieldFloat` / etc. over `GetField` / `SetField` to skip boxing.