# yomu

Kindle reader for `.epub` and `.txt`. The checkout directory is still `koxtch`.

## Books and UI

Open `.epub` and `.txt` only. Leave `.mobi`, `.azw3`, and `.xtch` unsupported.
Strings drawn by the C UI stay Japanese. `README.md` and both `INSTALL.txt` files stay English.

## Device

The device is a Paperwhite 11. `env_rotate()` in `src/kindle/kindle_fb.c` always returns 3. Ignore `YOMU_ROTATE`.
Sleep works only while the Kindle framework is running. Launch from the KUAL entry `yomu`. Leave the frontlight off.

## Source

Append a new field at the end of a struct. On `TypesetBook`, `faceIndex` stays after `hookCtx`.
The Makefile does not track header dependencies. After a header change or a C++ signature change, rebuild every translation unit: `rm -rf build/host && make test`. The Kindle binary is `rm -rf build/kindle && make kindle`. Host objects are compiled with `cc -Wall -Wextra -Werror`.
A new parameter changes the C++ mangled name. A default argument does not keep the old symbol. Recompile every caller.

`reference/koreader` and cookbook-jp are reference only. Do not link them. Do not bump the vendored FreeType. Do not commit commercial font files.

The body face list has no gothic row, no Code2000 row, and no `標準` face row. The size step labeled `標準` stays. The menu face stays gothic.

## Git

A commit subject is one short imperative sentence ending with a period. The body is empty. Commit only when asked.
