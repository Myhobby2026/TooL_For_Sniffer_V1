# ADR 0002: Integer SampleIndex as Primary Key

## Status
Accepted

## Context
Timing in digital logic analyzers is often stored as floating-point timestamps (seconds or microseconds). In multi-hour captures consisting of billions of samples, floating-point numbers accumulate precision drift, produce non-deterministic equality comparisons, and make gap detection brittle.

## Decision
Use `SampleIndex` (a strong 64-bit integer type) as the primary index and key for all sample blocks, trigger positions, bookmarks, and protocol events. Time is derived only when displaying to the user, calculated as an exact rational fraction `RationalTime` (`numerator / denominator`) using integer Euclidean descent arithmetic.

## Consequences
- Monotonicity checks and gap detections are exact integer comparisons (`lhs < rhs`).
- Multi-gigabyte captures remain bit-exact across platforms without IEEE-754 rounding differences.
- Golden test fixtures are completely deterministic.
