# glib 0.18.5 security backport

Tauri's GTK3 stack still requires glib 0.18. Updating only this crate to 0.20
would create incompatible GLib wrapper types. This directory retains the original
MIT-licensed crates.io package and applies exactly the two-line upstream fix for
[RUSTSEC-2024-0429](https://rustsec.org/advisories/RUSTSEC-2024-0429.html).

Upstream archive SHA-256:
`233daaf6e83ae6a12a52055f568f9d7cf4671dabb78ff9560ab6da230ce00ee5`.

Source fix: gtk-rs/gtk-rs-core PR 1343, merge commit
`05dff0ee696f9bcd8617cd48c4b812d046d440cb`. In `src/variant_iter.rs`, make
the output pointer mutable and pass `&mut p`, not `&p`, to the variadic C call.
The Cargo patch keeps the upstream version number; it does not pretend to be an
official fixed release. Remove this backport when the desktop stack upgrades to
a fixed upstream GLib version. License and original sources are retained.
