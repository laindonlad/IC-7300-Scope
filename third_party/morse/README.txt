The CW, RTTY, NAVTEX and WEFAX decoders from the Morse project (src/digi and src/morse_decoder.h), at its
commit a3465c9. PSK is here because the engine needs it, though the console doesn't offer it.

The one change: NavtexDemod::isChar in digi/navtex.h counts its bits with a loop, as Visual Studio has no
__builtin_popcount.
