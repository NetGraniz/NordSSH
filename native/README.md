# NordSSH native engineering workspace

This is the new C++20 implementation. There is no runnable SSH application or
native user release here yet. The web prototype lives outside this directory and
is not used by these targets.

Current product modules:

- `nordssh_vault_format`: bounded UTF-8 password and binary-header admission,
  exact AAD construction.
- `nordssh_vault_crypto`: AES-256-GCM/Argon2id authenticated in-memory envelopes,
  cleansed move-only secrets, fresh-nonce saves and device-key opening. Optional
  `NORDSSH_BUILD_CRYPTO=ON` requires OpenSSL 3.5.9; Argon2 source is hash-pinned.
  JSON schema validation, disk persistence and an unlocked product model are not
  implemented by this layer. Never use authenticated bytes as validated records.
- `nordssh_input`: one paste event route for a future terminal widget. Tests send
  synthetic Qt events; they do not inspect the user's clipboard or prove actual
  keyboard-layout/IME behavior on a desktop session.

Build the dependency-free core with CMake and a C++20 compiler:

```sh
cmake -S native -B native/build-core-clean -DCMAKE_BUILD_TYPE=Debug
cmake --build native/build-core-clean --config Debug
ctest --test-dir native/build-core-clean -C Debug --output-on-failure
```

`NORDSSH_BUILD_QT_INPUT=ON` additionally requires the pinned Qt SDK and the
corresponding `CMAKE_PREFIX_PATH`. Test executables need the SDK runtime on their
process search path. This is a developer setup, not the end-user deployment plan.
See [toolchain notes](../docs/NATIVE_TOOLCHAIN.md) and the
[acceptance matrix](../docs/NATIVE_TEST_MATRIX.md).

## Rejected experiment

`experiments/libvterm` is retained only to document a failed feasibility choice.
Its happy-path tests passed, but source review found security-blocking parser and
resource-management paths. It must not consume SSH/server output. The default
build does not fetch, compile or link it. Its opt-in experimental target is not a
product terminal and cannot satisfy a release gate.

The next engine candidate is pinned libghostty-vt with a small C++ adapter. Its
C API is explicitly unstable. Successful upstream CI or a local static-library
build will not by itself close our renderer, keyboard, memory or SSH gates.
