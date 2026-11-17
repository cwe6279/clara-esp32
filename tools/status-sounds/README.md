# Status sounds

Original droid-style chirps for a headless character: sine sweeps with vibrato, quick pitch hops
("droid talk"), a ring-modulated burble and a click. 24 kHz mono WAVs, peaks at -6 dBFS, none
longer than 0.7 s so they finish before she listens again. `python make_sounds.py` rebuilds them.

| Sound | For |
|---|---|
| wake | heard her name |
| listening-ends | stopped listening |
| thinking | thinking, or a photo uploading |
| shutter | the camera takes a photo |
| recording-start / recording-stop | a voice note starts / ends |
| happy | something went well |
| question | she needs an answer |
| error | something failed |
| connected | back on Wi-Fi or with the host |
| chatter | personality |
| sad | disappointed |

Not wired into the firmware yet: they go on the board with the speaker (MAX98357A). Rules then:
never during a voice note, and finished before listening opens, so the mic does not hear them.
