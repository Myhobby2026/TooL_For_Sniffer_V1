# ADR 0003: USB CDC Transport as Initial Communication Channel

## Status
Accepted

## Context
Teensy 4.1 supports USB High-Speed (480 Mbit/s). Communication with the desktop host can use standard USB CDC (Communications Device Class / virtual COM port), custom WinUSB/libusb vendor bulk endpoints, or Ethernet. WinUSB requires driver installation or INF configurations on Windows, while CDC is natively supported without third-party drivers on Windows 10/11, macOS, and Linux.

## Decision
Implement USB CDC (`usb_serial`) as the primary transport mechanism in Phase 1–3 behind the `ITransport` abstraction. Vendor bulk and Ethernet support remain planned extensions behind the same interface.

## Consequences
- Out-of-the-box driverless connection on all supported host operating systems.
- CDC payload bandwidth (~10–20 MB/s measured) governs the continuous digital sampling envelope.
- `ITransport` abstracts the underlying stream so future custom USB bulk drivers require zero changes to the capture pipeline or codecs.
