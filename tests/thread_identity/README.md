# Guest thread identity

Serious Sam's XAPI `GetCurrentThreadId` reads `fs:[0x28]`, then the
`UniqueThread` field at offset `0x12C`. Its engine uses this value as the owner
of recursive locks. A shared zero ID lets a worker enter a lock already held
by another thread.

This test loads a synthetic XBE with a TLS directory through the real memory
loader, then starts three native workers through the real guest TIB allocator.
It checks nonzero, distinct IDs, private current-thread records, preservation
of the legacy shared RW pointer, private SEH heads, and exclusion between the
main thread and a worker using the engine's recursive-lock decision.

No game data is required. The test covers images with a TLS directory; the
allocator's existing no-TLS behavior is outside this regression.

```powershell
cmake -S tests/thread_identity -B build/thread_identity -A x64
cmake --build build/thread_identity --config Release --target thread_identity_test
ctest --test-dir build/thread_identity -C Release --output-on-failure
```
