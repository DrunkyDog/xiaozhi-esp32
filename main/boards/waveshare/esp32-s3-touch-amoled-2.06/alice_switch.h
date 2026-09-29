#ifndef ALICE_SWITCH_H
#define ALICE_SWITCH_H

// Dual-firmware support for the alice partition layout (partitions/v2/alice_32m.csv):
//   Watch = ota_0/ota_1 (default boot), AI Voice = ota_2/ota_3 (this firmware).
// Everything here is a no-op on the regular single-firmware layouts.
namespace alice {

// True when this firmware runs from the AI Voice pair of the alice layout.
bool IsAliceLayout();

// Call once at boot. Returns to the Watch immediately if the Watch's Battery Saver paused
// AI Voice (NVS alice/voice_paused), otherwise arms a timer that marks this image valid and
// points the next boot back at the Watch, so any reset or power cycle lands on the Watch.
void Begin();

// Boots the Watch now (restarts). Returns only if no valid Watch image was found.
void ReturnToWatch();

}  // namespace alice

#endif  // ALICE_SWITCH_H
