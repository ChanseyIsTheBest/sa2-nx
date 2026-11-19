// Short video recordings of the game image for testing, from Renut's presented frame (renut::gpu::CaptureFrame), so
// they work with the window minimized and nothing can cover them. The same file format as banjo-nx's recorder:
// tools/make_video.sh turns the raw file into an MP4 and a contact sheet; tools/compare_raw.py compares two runs.
// sa2-nx's (1.0.0-1.0.6); sa2-nx-ult (2026-11-19): Renut's path only (sa2_capture.cpp).

#pragma once

#include <cstdint>
#include <functional>

namespace rex::ui {
struct RawImage;
}

namespace sa2::capture {

// Starts the recorder thread when sa2_video_s > 0, with the presented frame's source (Renut's CaptureFrame).
void StartWithSource(std::function<bool(rex::ui::RawImage&)> grab);

// Lockstep (sa2_video_lockstep, sa2_video_every = 1): Renut's Swap hook asks for the capture of the frame it is
// queueing (RenutLockstepOn), waits for that frame's image and hands it over here with the game frame it ends; this
// returns once the recorder has taken it, so no frame is skipped or taken late and two runs label the same frames the
// same, however loaded the PC is.
bool RenutLockstepOn();
void RenutLockstepFrame(uint64_t game_frame, const rex::ui::RawImage* image);

// Stops it (writes what it has, if anything).
void Stop();

}  // namespace sa2::capture
