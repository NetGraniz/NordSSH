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
ssh_archive="$task_root/libssh-0.12.2.tar.xz"
curl --fail --location --proto '=https' --tlsv1.2 \
  'https://www.libssh.org/files/0.12/libssh-0.12.2.tar.xz' --output "$ssh_archive"
printf '%s  %s\n' '49560f677d96e3706a904ac2de1116e25f3680937d51e5c92198fcba4a1c1e9f' "$ssh_archive" | sha256sum --check --status
tar -xf "$ssh_archive" -C "$task_root"
cmake -S "$task_root/libssh-0.12.2" -B "$task_root/libssh-build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$task_root/libssh-prefix" \
  -DOPENSSL_ROOT_DIR="$task_root/prefix" -DWITH_SERVER=OFF -DWITH_GSSAPI=OFF \
  -DWITH_ZLIB=OFF -DWITH_EXAMPLES=OFF -DWITH_PCAP=OFF -DWITH_DEBUG_CALLTRACE=OFF \
  -DWITH_EXEC=OFF -DWITH_NACL=OFF -DBUILD_SHARED_LIBS=ON
cmake --build "$task_root/libssh-build" --parallel 2
cmake --install "$task_root/libssh-build"
python3 -m venv "$task_root/test-python"
"$task_root/test-python/bin/python" -m pip install --disable-pip-version-check 'asyncssh==2.24.1'
cmake -S native -B native/build-crypto-ci -DCMAKE_BUILD_TYPE=Debug \
  -DNORDSSH_BUILD_CRYPTO=ON -DOPENSSL_ROOT_DIR="$task_root/prefix" \
  -DNORDSSH_BUILD_SSH=ON -DCMAKE_PREFIX_PATH="$task_root/libssh-prefix" \
  -DPython3_EXECUTABLE="$task_root/test-python/bin/python" \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build native/build-crypto-ci --parallel 2
LD_LIBRARY_PATH="$task_root/libssh-prefix/lib:$task_root/prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ctest --test-dir native/build-crypto-ci --output-on-failure
