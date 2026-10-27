"""Self-check for tools/nid_from_protos.json (produced by nid_harvest.py).

Every entry must satisfy the definition compute_nid(name) == nid. That is exactly what
made each entry admissible (a symbol name whose hash equals a real imported NID), so this
re-verifies the whole file with no game binary -- and trips if compute_nid ever regresses
or the JSON is hand-edited wrong.

Run: python tools/test_nid_from_protos.py
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nid_database import compute_nid, NIDDatabase

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "nid_from_protos.json")


def main() -> int:
    db = json.load(open(DATA, encoding="utf-8"))

    total = 0
    for library, funcs in db.items():
        assert isinstance(funcs, dict), library
        for name, nid_str in funcs.items():
            nid = int(nid_str, 16)
            got = compute_nid(name)
            assert got == nid, f"{library}::{name}: json 0x{nid:08X} != compute 0x{got:08X}"
            total += 1

    assert total >= 300, f"expected the harvested set, got only {total} entries"

    # And the database actually surfaces them.
    ndb = NIDDatabase()
    ndb.load_builtins()
    sample_lib = next(iter(db))
    sample_name, sample_nid = next(iter(db[sample_lib].items()))
    res = ndb.lookup_nid(int(sample_nid, 16))
    assert res and res[1] == sample_name, res

    print(f"ok: {total} proto-harvested NIDs all satisfy compute_nid(name)==nid "
          f"and load into the database")
    return 0


if __name__ == "__main__":
    sys.exit(main())
