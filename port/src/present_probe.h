#pragma once
// See present_probe.mm for the env knobs. Both functions are no-ops unless MGSV_PROBE=1.
#ifdef __cplusplus
extern "C" {
#endif
// Call on the UI thread after the window exists (ReXApp::OnPostSetup).
void MgsvPresentProbeInstall(void);
// Call from OnShutdown; joins the compositor-test thread and prints call totals.
void MgsvPresentProbeShutdown(void);

// Host-only bring-up aid: a Core Animation view on top of the game window whose
// contents are a brightened copy of Presenter::CaptureGuestOutput(). This does
// not use the presenter's UI-drawer path (which does not reach the presented
// swapchain on this SDL backend), so it proves the guest is rendering.
// MgsvCaptureViewCreate() must be called on the UI thread; Update() may be
// called from any thread (the layer update is dispatched to the main queue).
void MgsvCaptureViewCreate(void);
void MgsvCaptureViewUpdate(const void* rgba, int width, int height, int stride);
#ifdef __cplusplus
}
#endif
