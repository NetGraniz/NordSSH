"""Independent Python JSON grammar cross-check; synthetic records only, no vault I/O."""
import json
import random
import subprocess
import sys


def main():
    rng = random.Random(0x4E4F5244)
    alphabet = ["", "ascii", "привет", "😀", "e\u0301", "\0\n\t", '\\"/', "<img>"]

    def value(depth):
        leaf = rng.choice([None, True, False, -9007199254740991, 9007199254740991,
                           rng.randrange(-100000, 100000), rng.choice(alphabet)])
        if depth == 0:
            return leaf
        choice = rng.randrange(4)
        if choice == 0:
            return [value(depth - 1) for _ in range(rng.randrange(5))]
        if choice == 1:
            return {f"{i}-{rng.choice(alphabet)}": value(depth - 1)
                    for i in range(rng.randrange(5))}
        return leaf

    base = dict(schema=1, hosts=[], groups=[], credentials=[], known_hosts=[], snippets=[], settings={})
    documents = []
    for _ in range(500):
        candidate = dict(base, settings={"synthetic": value(5)})
        documents.append(json.dumps(candidate, ensure_ascii=bool(rng.randrange(2)),
                                    separators=(",", ":")).encode("utf-8"))
    known_valid = len(documents)
    seed = json.dumps(base, separators=(",", ":")).encode("ascii")
    for _ in range(10000):
        mutated = bytearray(rng.choice(documents[:known_valid] + [seed]))
        for _ in range(rng.randrange(1, 4)):
            offset = rng.randrange(len(mutated))
            mutated[offset] = rng.choice([b for b in range(256) if b not in (10, 13)])
        documents.append(bytes(mutated))

    result = subprocess.run([sys.argv[1], "--synthetic-lines"], input=b"\n".join(documents) + b"\n",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20, check=True)
    verdicts = result.stdout.splitlines()
    assert len(verdicts) == len(documents), "Oracle protocol result count"

    def pairs(items):
        output = {}
        for name, item in items:
            if name in output:
                raise ValueError("duplicate key")
            output[name] = item
        return output

    def no_noninteger(_):
        raise ValueError("noninteger")

    admitted = 0
    for index, (document, verdict) in enumerate(zip(documents, verdicts)):
        assert verdict in (b"0", b"1"), "Unexpected oracle output"
        if index < known_valid:
            assert verdict == b"1", f"Valid synthetic document rejected at case {index}"
        if verdict != b"1":
            continue
        admitted += 1
        decoded = json.loads(document.decode("utf-8", errors="strict"), object_pairs_hook=pairs,
                             parse_float=no_noninteger, parse_constant=no_noninteger)
        assert isinstance(decoded, dict) and set(decoded) == set(base), "Root shape mismatch"
        assert type(decoded["schema"]) is int and decoded["schema"] == 1, "Version mismatch"
        assert isinstance(decoded["settings"], dict), "Settings mismatch"
        for name in ("hosts", "groups", "credentials", "known_hosts", "snippets"):
            assert isinstance(decoded[name], list) and all(isinstance(item, dict) for item in decoded[name]), "Record shape mismatch"
    print(f"Independent JSON oracle: {len(documents)} synthetic cases, {admitted} admitted; not record-schema or persistence proof")


if __name__ == "__main__":
    main()
