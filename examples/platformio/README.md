# PlatformIO Arduino Examples

LibOSDP provides a native port for [PlatformIO][1]. If you have it already
setup, you can set `lib_deps=https://github.com/osdp-dev/libosdp` in your
`platformio.ini` and build your CP/PD application as you would build any
PlatformIO project.

Note: These examples are provided only for demonstration purposes. A real world
CP/PD device would have to do a lot more.

Commands and events are queued *by reference*, so a submitted object must stay
alive and unmodified until its completion callback hands ownership back. These
sketches show how to honour that without a heap: a single static command/event
plus an in-flight flag that the completion callback clears, so the object is
never refilled while LibOSDP still holds it. The `examples/c` samples show the
allocate-per-submission variant for platforms that have an allocator.

See the [Build and Install guide](https://doc.osdp.dev/libosdp/build-and-install)
on doc.osdp.dev for more on building and consuming LibOSDP.

[1]: https://docs.platformio.org/en/latest/what-is-platformio.html
