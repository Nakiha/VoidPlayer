# macOS Native ABI Contract

The macOS native bridge is embedded by the Flutter runner. It is not a
system-wide plugin ABI, but the C boundary still needs explicit rules because it
crosses Swift, C++, Objective-C++, Metal, and CoreVideo lifetimes.

## Versioning

`VP_MACOS_NATIVE_API_VERSION` identifies incompatible layout or ownership
changes. Versioned structs start with:

```c
uint32_t struct_size;
uint32_t api_version;
```

`VPMacOSNativeFrameInfo` is currently versioned because it crosses synchronous
and asynchronous callback/upload paths. New cross-boundary structs should use
the same prefix before being consumed by Swift or another binary boundary.

## Return Values

Functions returning `int` use `0` for success. Negative values are failures and
must map to `VPMacOSNativeStatus` categories where possible. Existing legacy
functions may still collapse failures to `-1`; do not introduce new positive
error values for player APIs.

Uploader validation has its own positive status enum because it is queried as a
diagnostic classifier, not as a generic success/failure result.

## Ownership

`VPMacOSNativePlayer*` and `VPMacOSMetalPresentationBackend*` are owned by the
caller and freed by their matching `Destroy` functions. Null destroy/clear calls
are valid no-ops.

`const char*` arguments are borrowed for the duration of the call.

`void* pixel_buffer` arguments are borrowed `CVPixelBufferRef` values. Native
does not retain them for the installed presentation target; Swift must keep the
draw buffer alive until it is replaced or
`VPMacOSNativePlayerClearMetalPresentationTarget` returns. Per-frame
CVPixelBuffer snapshots used by the renderer remain owned by their frame
storage.

## Threading

Player methods may be called from the main thread or automation thread. Calls
for the same player are serialized at the player boundary, but callers should
not make concurrent control calls unless the operation explicitly documents that
pattern.

The frame callback may run on a renderer/native worker thread. It must not call
back into player APIs synchronously. Clearing the callback waits for callback
invocations already in progress to return. Destroy clears and drains the
callback before freeing the player.

## Validation

`macos_native_abi_smoke` enforces the current ABI invariants in CTest:

- versioned struct prefix and initialization for `VPMacOSNativeFrameInfo`;
- status enum sign conventions;
- standard-layout/trivial C ABI structs;
- null-safe destroy/clear callback boundaries.
