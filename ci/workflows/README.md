# GitHub Actions CI Workflows

These workflow configurations implement the CI specifications described in `docs/architecture_review.md` section 18.2 (P1.14):

- `ci-core.yml`: Headless matrix CI testing across Linux (GCC, Clang), Windows (MSVC), macOS (Apple Clang), plus AddressSanitizer and ThreadSanitizer jobs.
- `ci-gui.yml`: Qt 6 GUI build and offscreen smoke tests.
- `ci-firmware.yml`: Teensy 4.1 firmware cross-compilation check using `arm-none-eabi-gcc`.

To enable these in GitHub Actions, copy them to `.github/workflows/` using a token with the `workflows` permission.
