# LibOSDP Usage Samples

These samples are meant to act as a reference for the API calls of LibOSDP. They
demonstrate the right initialization and refresh workflows the CP/PD to work
properly but are not working examples.

They also show the ownership rules that a production application must follow:
commands and events are queued *by reference*, so a submitted object has to
outlive the call and is handed back to the application -- exactly once -- through
its completion callback. The samples allocate per submission and free in that
callback, which is the simplest pattern that never leaks and never frees an
object the library still holds. Advanced uses (file transfer, smartcard/PIV,
biometrics) are left to the documentation.

Assuming you have already built LibOSDP, you can run the following commands to
to compile `cp_sample` and `pd_sample`.

```sh
gcc cp_app.c -o cp_sample -l osdp -I ../../include/ -L ../../build/lib
gcc pd_app.c -o pd_sample -l osdp -I ../../include/ -L ../../build/lib
```

See the [C API reference](https://doc.osdp.dev/api/) and the
[Build and Install guide](https://doc.osdp.dev/libosdp/build-and-install) on
doc.osdp.dev for the full API and build details.
