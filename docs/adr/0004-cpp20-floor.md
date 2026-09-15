# ADR 0004: C++20 Language Standard Floor

## Status
Accepted

## Context
While C++23 introduces convenient features such as `std::expected` and `std::print`, compiler support for C++23 varies significantly across common CI distributions (e.g. GCC 12 vs 13+, MSVC versions). Declaring C++23 as a mandatory baseline would preclude building on GCC 12.2 and older enterprise environments.

## Decision
Establish **C++20** as the mandatory language standard floor (`cxx_std_20`). Features like `std::expected` are replaced with a lightweight, tested `usn::StatusOr<T>` with an identical API surface, enabling trivial aliasing in the future when toolchains advance.

## Consequences
- Guaranteed compilation on GCC 12+, Clang 15+, and MSVC 19.30+.
- No dependency on experimental or unstable compiler flags.
- Forward-compatible error handling via `Status` / `StatusOr`.
