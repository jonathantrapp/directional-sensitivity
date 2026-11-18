# User-space preview implementation

Scope: the live preview and the automated tests of its value-state and parser behaviour. The tests do not launch the window or use mouse hardware. The kernel driver and its ABI are untouched.

## Data path

`WM_INPUT -> fixed current-event buffer -> selected-device model -> Processor -> numeric display`

Optional `--buffered` drains at most four 64 KiB buffers after the current event. Generation of a `WM_INPUT` message does not mean that its event is still present in the buffered queue, so the current event is always read separately. The current event then goes through `DefWindowProcW` cleanup. Buffered records are bounds-checked and traversed using native alignment, with mouse data copied into an aligned local value before processing. Buffer failure stops the preview; it does not silently lose a report and continue.

The supplied `uint64_t` storage meets native record alignment. WOW64 buffered processing needs additional layout handling and is rejected instead of claiming support. Standard non-buffered mode remains available. No input registration uses `RIDEV_INPUTSINK` or `RIDEV_NOLEGACY`; the app observes foreground reports and leaves normal input delivery alone.

The model filters by the selected device handle and ignores non-mouse reports. Start also rechecks the saved path/handle against current enumeration, preventing a stale selection from silently choosing a different mouse. Selected-device removal clears selection; Refresh does not auto-select a replacement.

On focus loss, removal, changed attributes, absolute input, read/format errors or total-capacity limits, preview processing stops and clears carry. Start explicitly resets the processor and numeric totals. Unknown DPI changes cannot be detected reliably and remain the user's responsibility. Only a profile matching the supplied DPI/reference sensitivity can reach the window.

The UI timer refreshes text around 30 times a second. The report path allocates nothing, writes no files/logs, reads no per-packet clocks, and never paints. The application has no driver-control or injection path. All state is confined to the window thread, so it needs no per-report locks.

## Tests and limits

`preview_tests` uses fake handle tokens and synthetic bytes. It verifies selected-device isolation, state resets, cardinal carry freeze, exact processing, absolute/attribute stops, total-overflow protection, unaligned source parsing, truncated/oversized records, native padding and non-mouse skipping. It opens no window, registers no devices and calls no capture API.

These tests do not verify Win32 event ordering, registration, focus behavior, actual removal messages, high-rate queue behavior, or UI layout. The README supplies a human test checklist for those points. Kernel safety, signing, compatibility and deployment remain outside this handoff.

## API references checked during implementation

- [Using Raw Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/using-raw-input): current-event read followed by optional queue drain and default-window cleanup.
- [GetRawInputBuffer](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getrawinputbuffer): variable-size record traversal, alignment and WOW64 distinctions.
- [DefRawInputProc](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-defrawinputproc): does not perform per-record cleanup; the preview's current event uses DefWindowProcW.
