# Run Tests

`make check` on cmake builds will invoke pytest correctly. During development,
it might be useful to run an individual test (instead of everything). To do so,

```
PYTHONPATH=../../build/python/  python3 -m pytest -vv -s test_events.py::test_event_input
```

For the full release-style test flow, see
[Running the Test Suite](https://doc.osdp.dev/libosdp/build-and-install#running-the-test-suite)
on doc.osdp.dev.
