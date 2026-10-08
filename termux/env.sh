# shared settings for the termux scripts; source, don't run
NORNS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NORNS_DEPS="${NORNS_DEPS:-$HOME/norns-deps}"
NORNS_LOG="${NORNS_LOG:-$TMPDIR/norns-run}"
export PATH="$NORNS_DEPS/prefix/bin:$PATH"
