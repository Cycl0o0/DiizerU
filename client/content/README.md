# content/ — bundled into the .wuhb (mounted at /vol/content)

Goes inside the `.wuhb` and is read-only at runtime. Future assets:
- `cacert.pem` (TLS roots for libcurl when talking to the relay)
- `font.ttf` (UI font for SDL2_ttf)

Empty placeholders for the M3 hello build (which draws shapes, no font yet).
