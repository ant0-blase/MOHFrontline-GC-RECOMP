# Framework source origin

This independent, flattened framework snapshot was originally created on
2026-08-11 from these source revisions:

- ModernGekko: `d4c030ec7f5b`
- RecompCore runtime: `eeaad721ded6`
- DolRecomp: `e7d677ab58ee`

All initialized external dependency contents were copied into this tree. Git
submodule metadata, pre-migration build directories, and loose `*.pre-*`
backup files were deliberately excluded.

The local framework includes a four-job cap in `moderngekko-port` so module
generation and compilation do not exhaust memory on large recompilation jobs.
Game-specific changes for Medal of Honor: Frontline should stay outside the
framework whenever possible so generic runtime fixes remain easy to identify.
