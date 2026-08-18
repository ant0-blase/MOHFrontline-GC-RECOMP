# Extract your GMFE69 game here

This directory is intentionally empty in the repository.

You may either let `../build.sh` extract your own USA GameCube ISO automatically, or place an already extracted `GMFE69` filesystem here with at least:

```text
extracted/
├── sys/
│   ├── boot.bin
│   └── main.dol
└── files/
    └── ...
```

The build recursively discovers every `.dol` and `.elf` under this directory and recompiles every unique executable image into the same native module. Non-stripped ELF function symbols are also recovered automatically and supplied to DolRecomp as function maps.

Do not commit extracted game files.
