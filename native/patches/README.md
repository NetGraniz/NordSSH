# Ghostty VT dependency patch

`ghostty-tinyio-readlink.patch` applies only to commit
`08a9518340e19487bd5fdf96963583fb32f0c41f`. Run
`pwsh -File native/scripts/patch-ghostty.ps1 -Source <fresh-checkout>` before
building the SDK. The script rejects another commit or tracked local changes.
Do not apply it over an existing SDK checkout with work to preserve.

The pinned TinyIo fd real-path fallback returned the number of bytes from
`readlink` even when the caller's buffer truncated the path. The Linux gate
caught this in `dirRealPathFile edge cases`. Linux documents silent truncation:
[readlink(2)](https://man7.org/linux/man-pages/man2/readlink.2.html).

The patch resolves into a fixed PATH_MAX scratch buffer, rejects ambiguous full
scratch results and undersized output before copying. Exact-fit output buffers
still work. A Linux regression covers zero, eight-byte, one-byte-short and
exact-fit outputs through file and non-cwd directory handles, with guard bytes.
No tests are removed or marked expected failures. No automatic upstream update
is performed. Re-evaluate and remove this patch when repinning to a fixed source.

This is a narrow dependency correctness fix, not proof of parser safety or a
completed cross-platform terminal gate. Linux results must be recorded after CI.
