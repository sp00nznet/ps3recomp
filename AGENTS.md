# Notes for contributors and coding agents

These apply to everyone who changes this repository, people and coding agents alike.
[CONTRIBUTING.md](CONTRIBUTING.md) has the full guide; this file collects the rules that are
easy to miss.

## Opt-in flags

[docs/OPT_IN_FLAGS.md](docs/OPT_IN_FLAGS.md) lists every switch the code reads: environment
variables in the runtime, libraries, project template and tools, and CMake options and cache
settings. Whenever a change adds, renames or removes one, or changes its default:

- update its row in `docs/OPT_IN_FLAGS.md` in the same change, with its kind, default, class
  (`feature`, `experimental` or `diagnostic`), what it does and where it is read;
- run `python tools/check_opt_in_flags.py`. It fails on a switch missing from the document, on
  a row nothing reads any more, and on an environment read whose name it cannot see (add those
  to `DYNAMIC_READS` or `WRAPPERS` in the script, with the names they read).

CI runs the same check through `tools/test_opt_in_flags.py`.

## Before opening a pull request

Follow "Pull Request Guidelines" in [CONTRIBUTING.md](CONTRIBUTING.md): one logical change per
pull request, test evidence in the description, and no game binaries, encryption keys,
proprietary data or lifter output in the tree.
