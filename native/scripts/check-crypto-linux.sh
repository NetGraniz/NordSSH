#!/usr/bin/env bash
# Isolated engineering test dependency, not a product installer/runtime bundle.
set -euo pipefail
task_root="$(mktemp -d)"
source_archive="$task_root/openssl-3.5.9.tar.gz"
curl --fail --location --proto '=https' --tlsv1.2 \
  'https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz' \
  --output "$source_archive"
printf '%s  %s\n' '603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a' "$source_archive" | sha256sum --check --status
tar -xzf "$source_archive" -C "$task_root"
pushd "$task_root/openssl-3.5.9"
./Configure shared no-tests no-docs no-legacy no-engine no-dso no-asm --prefix="$task_root/prefix" --libdir=lib
make -j2
make install_sw
popd
cmake -S native -B native/build-crypto-ci -DCMAKE_BUILD_TYPE=Debug \
  -DNORDSSH_BUILD_CRYPTO=ON -DOPENSSL_ROOT_DIR="$task_root/prefix" \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build native/build-crypto-ci --parallel 2
LD_LIBRARY_PATH="$task_root/prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ctest --test-dir native/build-crypto-ci --output-on-failure
