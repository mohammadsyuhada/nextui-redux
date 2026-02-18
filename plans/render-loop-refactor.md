# Render Loop Refactor: Event-Driven / Vsync-Driven Flow

## Background

In `workspace/all/nextui/nextui.c`, the main render loop uses fixed `SDL_Delay()` calls to pace rendering. This was changed from `SDL_Delay(100)` to `SDL_Delay(16)` as an interim fix, but the underlying approach (polling with fixed sleeps) is still suboptimal.

### Current locations using fixed delays

- **Line ~1338** (animation branch, nothing-to-draw path): `SDL_Delay(16)` — was 100ms
- **Line ~1353** (idle path, nothing needs drawing): `SDL_Delay(17)`

### Problems with fixed delays

1. **Frame drift**: Work done before the delay isn't accounted for. If drawing takes 4ms + 16ms sleep = 20ms total, the loop runs at 50fps instead of 60fps.
2. **Wasted wakeups**: The thread wakes every 16ms just to check flags (`dirty`, `getAnimationDraw()`, `thumbchanged`, etc.) and often goes right back to sleep.
3. **Input latency**: User input can wait up to 16ms before being processed (worst case).

---

## Option 1: Event-Driven with `SDL_WaitEventTimeout`

### Concept

Replace blind `SDL_Delay` with `SDL_WaitEventTimeout`, which blocks until either an event arrives or a timeout expires.

### How it works

```c
// Instead of:
SDL_Delay(16);

// Use:
SDL_Event event;
if (SDL_WaitEventTimeout(&event, 16)) {
    // An event arrived — process it immediately
    handleEvent(&event);
}
// Then check render state and draw if needed
```

### Benefits

- **Zero wakeups when idle**: If nothing happens for 5 seconds, the thread sleeps the entire time instead of waking ~312 times.
- **Instant input response**: Button presses wake the thread immediately, no waiting for the sleep to expire.
- **Battery friendly**: Significantly fewer CPU wakeups when the UI is static.

### What needs to change

- The input handling (currently in a separate thread via `SDL_PollEvent`) would need to be merged into the render loop, or the render thread needs a way to be signaled by the input thread.
- The `dirty` flag mechanism could be replaced with `SDL_PushEvent` to wake the render thread when state changes.
- Animation timers would need to push events or use `SDL_AddTimer` to generate wakeup events at the right intervals.

### Complexity

Medium. The main challenge is restructuring the input/render thread relationship. Currently input runs in its own thread and sets flags — this would need to also post an event to wake the render thread.

---

## Option 2: Vsync-Driven Flow

### Concept

Let the display hardware's vertical blank signal drive frame pacing instead of software timers.

### How it works

```c
// No SDL_Delay at all. The flip itself blocks until vsync.
// Render loop becomes:
while (running) {
    processInput();
    if (needsRedraw()) {
        drawFrame();
        PLAT_GPU_Flip();  // Blocks until next vsync (~16.6ms at 60Hz)
    } else {
        SDL_WaitEvent(NULL);  // Sleep until input arrives
    }
}
```

### Benefits

- **Perfect frame pacing**: Every frame lands exactly on a display refresh. No tearing, no drift.
- **Zero CPU-side timing code**: The display drives the loop. No `SDL_Delay`, no `SDL_GetTicks` math.
- **Minimal latency**: Input is processed at the start of each frame, rendered, and displayed at the next vsync.

### What needs to change

- `PLAT_GPU_Flip()` must be configured for vsync (SDL_RENDERER_PRESENTVSYNC or platform-specific equivalent). Need to verify the device/platform supports this.
- The render loop needs to be single-threaded or carefully synchronized — vsync blocking on the render thread is the core timing mechanism.
- When idle (no animation, no input), the loop should block on events rather than spinning on vsync flips with identical frames.

### Complexity

High. Requires understanding the platform's GPU/display pipeline and confirming vsync support. The current multi-threaded architecture (separate input, background loading, animation threads) would need careful review to avoid deadlocks when the render thread blocks on vsync.

---

## Recommended Approach

A hybrid of both options:

1. **Use `SDL_WaitEventTimeout` for the idle case** (no dirty, no animations). This eliminates unnecessary wakeups when the UI is static.
2. **Use elapsed-time-compensated delays for active rendering**:
   ```c
   Uint32 frame_start = SDL_GetTicks();
   // ... render work ...
   Uint32 elapsed = SDL_GetTicks() - frame_start;
   if (elapsed < 16)
       SDL_Delay(16 - elapsed);
   ```
3. **Investigate vsync** as a longer-term improvement if the platform supports it reliably.

### Migration steps

1. Audit all `SDL_Delay` calls in the render path
2. Replace idle-path delays with `SDL_WaitEventTimeout`
3. Add elapsed-time compensation to active rendering delays
4. Add a signaling mechanism (e.g., `SDL_PushEvent` with a custom event) so background threads can wake the render thread when they finish loading thumbnails, backgrounds, etc.
5. Test thoroughly for frame drops, input latency, and battery usage

---

## References

- Current render loop: `workspace/all/nextui/nextui.c`, starting around the `if(dirty)` block (~line 513)
- Animation branch: ~line 1235
- Idle branch: ~line 1342
- SDL docs: https://wiki.libsdl.org/SDL2/SDL_WaitEventTimeout
