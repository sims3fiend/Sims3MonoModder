# ExampleMod

A showcase / template mod that touches most of the S3MM API in one file. It is **not** a real gameplay mod, this exists to be read and copy-pasted from.

## What it demonstrates

- **Hooks**: static, instance (`__this`), priority ordering, and `ref`/`out` params via `Hook.Ref<T>`.
- **Dispatch primitives**: `CallNext` (chain through), `CallOriginal` (skip the chain), and skip-original (return without calling either).
- **Typed / baked dispatch**: `Chain.CallNext` (bakes to zero-overhead forwarding) and `Chain.ByRef` (true ref/out forwarding, no array round-trip).
- **Value-type returns**: the exact-return-type rule, demoed on an enum return and a 16-byte struct return.
- **Generic targets**: hooking an inflated method nested in a generic type via `RegisterByPointer`.
- **Watchers**: read-only observation that runs ahead of all hooks.
- **Reflection cache**: `FindClass` / `FindField` / `FindMethod`, typed static & instance field accessors, reflection-free RTTI, and `Cache.Invoke`.
- **Dynamic hooks**: `Hook.RegisterByPointer` with no attribute.
- **Lifecycle**: `[OnLoad]` / `[OnUnload]` with state restore for clean hot-reload.
- **Console cheats**: `CommandSystem.RegisterCommand` (and why the delegate is
  rooted in a static field).
- **Audit API**: loaded mods, hook chains, replaced assemblies.

## Running it

Copy the ExampleMod.cs file to `Documents/Electronic Arts/The Sims 3/S3MM/Source/ExampleMod/`and load it. Then, at the cheat console (**Ctrl+Shift+C**):

- `examplemod`: dump ExampleMod's status to the log.
- `examplemod skip`: fire the skip-original demo on demand (gameplay *should* never trigger it naturally).
- `examplemod generic`: drive the generic-target hook on demand.