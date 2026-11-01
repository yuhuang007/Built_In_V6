# Offline Pinyin Decoder

Vendored libgooglepinyin from https://github.com/pkg-ime/libgooglepinyin,
commit 4a5016ac35ca5f1db4b56eadcf04ee47f00200f0 (Apache-2.0).
Original copyright and license headers are retained. Raw dictionary inputs under
assets/pinyin/source are from the same commit.

Local patches: enable model building only with P4_PINYIN_BUILD_MODEL; disable
personal dictionary loading/writing with P4_PINYIN_READ_ONLY in the file-path
initialization route. Production uses only this route, not the FD API.
The fuzzy comparison definition uses its declared int32 return type (int32_t
is long on the P4 toolchain); unused dictionary handles are zero-initialized.
Personal dictionary synchronization is excluded from the read-only build.

Dictionary serialization uses native size_t and structure layout. Build the
dictionary and host tests as 32-bit little-endian; do not use a 64-bit model on
ESP32-P4. Runtime verifies the exact dictionary size and SHA-256 before opening.
Personal learning is deliberately disabled: typed contents are not persisted.
