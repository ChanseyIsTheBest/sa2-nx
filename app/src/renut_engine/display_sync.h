// banjo-nx (2026-10-19, latency batch 3): the frame limiter locked to the display's refresh, and the input -> display
// measurement (renut_engine/display_sync.cpp).
//
// On the Switch the compositor shows a new image only at its 60 Hz refresh (there is no tearing mode; swap interval 0
// only stops the present from waiting). The frame limiter (frame_cap) released the game's frames on its own 33.3 ms
// timer, at a phase to the refresh that slid as the two clocks drifted: an image finished just after a refresh waited
// almost 16.7 ms more, and now and then one was shown for three refreshes and the next for one. Here a thread follows
// the display's vsync event (viGetDisplayVsyncEvent): the limiter releases each frame at a refresh plus a phase, and
// the phase is found by measurement (banjo_display_phase_auto): as late as the frame's image still makes the refresh it
// made before. A frame that ends late is released at once as before (no snapping to 20 fps).
#pragma once

#include <cstdint>
#include <string>

namespace rex::graphics {
// Port 172: sdk/src/graphics/graphics_system.cpp (banjo_vblank_from_display).
extern uint64_t (*g_display_refresh_wait)(uint64_t seen, uint64_t timeout_ns);
}

namespace banjo::display_sync {

// Port 172: the display's refreshes counted so far, once one past 'seen' has come or 'timeout_ns' has passed.
uint64_t WaitRefresh(uint64_t seen, uint64_t timeout_ns);

// Starts the thread on the first call (the Switch, banjo_display_sync); a no-op elsewhere and after the first call.
void Start();

// The next frame's release for the frame limiter, in std::chrono::steady_clock nanoseconds: the frame now ending at
// 'now', the previous one released at 'previous' (0: none), 'frame_ns' the limiter's frame time. 0 when the display's
// refresh is not known (the limiter keeps its own timer then). '*anchored': the release is a refresh plus the phase.
uint64_t NextRelease(uint64_t now, uint64_t previous, uint64_t frame_ns, bool *anchored);

// The limiter released the game's next frame (its Swap is rex::latency::SwapCount() + 1) at 'at'.
void NoteRelease(uint64_t at, bool anchored);

// The renderer presented the image of the guest's 'swap'-th Swap (after Video::Present).
void NotePresented(uint64_t swap);

// sa2-nx (2026-10-28, sa2_renut_queue_drain; ketsui-nx's ketsui_renut_queue_drain, passed on by futari-nx): with two
// swapchain images (pacman_renut_double_buffer) a frame's present waits for the image the display still shows, which it
// gives back only at the refresh the frame was meant for: one late image (a load, a slow frame) and every image after it
// is shown a refresh late (about 17 ms more input lag, still 60 fps), for as long as the game keeps up. The display
// thread counts the new images not finished at their refresh; after a second of them it asks for the game's next frame
// to be released one refresh later, once: the image before it makes its refresh, the queue empties. TakeHold: whatever
// paces the game takes that request (the display thread's frame start below, or a numeric frame_cap's limiter). This
// empties the queue only if the game then stays on the display's grid: with nothing pacing it the game catches up at
// once and the queue fills again (the second console run: 5 drains, every image still late). QueueDrainOn: both keys
// and banjo_display_sync.
bool QueueDrainOn();

// pacman-nx (2026-11-03, pacman_vblank_display_sync; futari-nx's futari_vblank_display_sync and futari_vblank_phase_auto):
// the display's refresh starts the game's frames. On the 360 a game is paced by the vertical blank; here nothing paced
// this one, and it ran until the render thread held it back, frames queued behind each other: 64 ms from input to
// display with 14 ms of work. At each refresh the display thread tests the newest image's fence and sets the phase, then
// after the phase gives a tick; the game's Swap (renutFrameLimit, pacman_keys.cpp) waits for it. Futari's vblank handler
// is not used: this game's frames do not count vblanks, so the wait is at the Swap. FramePacingOn: the key and
// banjo_display_sync. WaitForFrameStart: the game's thread, once a frame.
bool FramePacingOn();
void WaitForFrameStart();

bool TakeHold();
// The late input's extra margin (render_thread.cpp), in microseconds: 1 ms more at each image late at its refresh,
// 0.25 ms less after 180 in time (with pacman_renut_queue_drain), so late input cannot push the frames back into the
// late queue.
uint32_t LateInputGuardUs();

// One line for the 10 s report; empty when the thread does not run.
std::string Report();

}  // namespace banjo::display_sync
