# Golden files

Each case is a directory holding everything needed to reproduce it:

```
tests/golden/<case>/
    config.json    what the case describes, in plain data
    input.bin      the exact bytes the case feeds in (where applicable)
    expected.hex   the exact bytes the case must produce, one byte per hex pair
```

`expected.hex` uses `#` for comments and two lowercase hex digits per byte separated
by single spaces, so a diff points at the byte that changed rather than at a wall of
text.

## Why `wire_sample_block` is generated outside the C++ code

`tests/golden/wire_sample_block/expected.hex` was produced by an independent Python
implementation of the field layout and the CRC32C polynomial described in
`shared/wire/usn_wire.h`, not by running `PacketCodec::buildPacket()` and saving its
output. Saving the implementation's own output would make the golden file a change
detector only: it would keep passing no matter how wrong the encoder was, as long as
it was wrong in the same way twice.

The generator asserts the CRC32C check vector (`"123456789"` -> `0xE3069283`) before
it writes anything, and asserts the prefix length is 40 bytes and the header length
is 32, both of which come from the specification rather than from the code under
test. The C++ golden test then builds the same packet and compares byte for byte, so
the two implementations have to agree.

## Regenerating

Regeneration is a deliberate act, not something a test does on failure. Re-run the
generator, then read the diff and confirm every changed byte is a change you
intended:

```
python3 tests/golden/regenerate.py
git diff tests/golden
```

If the diff is not what you intended, the code is wrong; do not commit the new
golden.
