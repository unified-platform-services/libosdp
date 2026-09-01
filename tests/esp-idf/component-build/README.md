ESP-IDF component build test
============================

A build-only ESP-IDF project that proves `esp-idf/component.cmake` still
compiles and links. Nothing here runs on hardware; `main.c` exists to reference
enough of the public API that the linker must pull in the PD role, the channel
plumbing, and the platform glue in `esp-idf/src/osdp_esp.c`.

The release workflow gates the ESP Component Registry upload on this building
for one Xtensa and one RISC-V target, so a component that cannot compile never
reaches the registry.

Building it by hand
-------------------

`libosdp` is resolved from `components/libosdp`, which must be a symlink to the
repo root -- ESP-IDF derives a component's name from its directory basename, and
the checkout directory is named after the repository only by convention. The
link is generated rather than committed, so create it once:

```sh
mkdir -p components
ln -sfn ../../../.. components/libosdp
```

Then, from an ESP-IDF environment:

```sh
idf.py set-target esp32
idf.py build
```

The project promotes the library's warnings to errors (`-Werror`, applied to the
libosdp component only), which the component itself deliberately does not do.
