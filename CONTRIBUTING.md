# Contributing

## Build requirements

- GCC or Clang
- NASM
- Python 3
- GNU Make

## Build and test

```sh
make clean
make
make firmware/firmware.bin disk/disk.img
make test
```

The emulator accepts any compatible raw firmware image through `--rom=...`. The bundled firmware and disk image are sample inputs, not hard-coded requirements of the emulator core.

## Pull requests

Please keep CPU, memory, PCI and device changes separate where possible. Include:

- a short explanation of the architectural behaviour being implemented;
- a reproducible command or test case;
- whether the change affects the generic core or only a selected platform profile;
- updated documentation for new command-line options or device models.

Do not commit generated binaries, disk images, framebuffer dumps or build logs.
