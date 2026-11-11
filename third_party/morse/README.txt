The CW, RTTY, NAVTEX and WEFAX decoders from the Morse project (src/digi and src/morse_decoder.h), at its
commit a3465c9. PSK is here because the engine needs it, though the console doesn't offer it.

The one change: NavtexDemod::isChar in digi/navtex.h counts its bits with a loop, as Visual Studio has no
__builtin_popcount.

The console's change for CW (digi/engine.h, digi/streams.h): a CW lock, its channel and its stream are held
through CW_HOLD_SEC (2 minutes) of silence rather than LOST_SEC, as a QSO heard from one side only is quiet
while the other side sends; while it's quiet, a clear winner elsewhere can take the lock. A returning signal
is matched to where its channel last heard it as well as where it was found, so drift doesn't make it new.
