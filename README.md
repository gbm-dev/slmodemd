# slmodemd

Patched Smart Link software modem daemon for Linux.

This repository is focused on `slmodemd` only.

## Build
Requirements:
- Linux
- 32-bit userspace toolchain (`gcc -m32`) on 64-bit hosts

Build:
```bash
make
```

Binary output:
- `slmodemd/slmodemd`

## Runtime
Example:
```bash
./slmodemd/slmodemd -d9
```

If you are integrating with an external helper, use `-e <helper-path>`.

## License
Smart Link 3-clause BSD-style license (see `slmodemd/COPYING`).
