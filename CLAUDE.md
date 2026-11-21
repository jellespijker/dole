# Dole

## Building

Build with `bin/build`, or pass `-j"$(bin/jobs)"` to `cmake --build`. Never
use a bare `-j` or `-j$(nproc)`: with the Makefile generator a bare `-j` is
unlimited, and on a low-memory machine the compile thrashes until
systemd-oomd kills the whole terminal session, agents included. Run one
build at a time; do not build several `build-*` directories in parallel.

## Testing

`bin/test` builds, then runs every test offscreen; its arguments go to
`ctest`. Keep the build free of warnings and the tests free of QML warnings.

## Publishing

The repository is public. Never commit absolute paths from this machine
(`/home/<user>/…`), host names, Tailscale addresses, or credentials. Design
work in progress lives in `.impeccable/`, which stays out of git.
