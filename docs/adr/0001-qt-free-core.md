# ADR 0001: Qt-Free Core Architecture

## Status
Accepted

## Context
The project requires a cross-platform desktop UI (targeting Qt 6 / QML) along with high-throughput capture, decoding, and analysis pipelines. In many projects, Qt types (`QString`, `QByteArray`, `QObject`, `QVector`) permeate the entire codebase, making headless CI, unit testing without a display server, and embedded compilation of core primitives difficult.

## Decision
Enforce a strict "Qt-free seam" (Rule A):
- Everything below `desktop/app` (`usn::common`, `usn::model`, `usn::transport`, `usn::trigger`, `usn::hal`, `usn::protocol`, `usn::core`) has zero Qt dependencies.
- Qt 6 is introduced strictly at `desktop/app` (view-models and `QAbstractItemModel` wrappers) and `desktop/gui` (QML views).
- Enforced mechanically by CMake CTest `architecture.qt_free_core`.

## Consequences
- 80%+ of the project is unit-testable in headless CI in seconds.
- Core algorithms and codecs can be compiled across multiple compilers (GCC, Clang, MSVC) without needing Qt installed.
- Clear separation between presentation logic and capture engine logic.
