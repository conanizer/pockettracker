# The licence payload for the bash package scripts, sourced after $SRC is set. The list itself is
# shell/licenses.txt, which build-windows.ps1 reads too.

PT_LICENSES=$SRC/shell/licenses.txt

# The names that ship in licenses/, one per line.
licence_names() {
    awk '$1 == "file" { print $3 }' "$PT_LICENSES"
}

# Copies every `file` line into a package's licenses/ folder.
copy_licences() {   # destination folder
    local from name
    while read -r from name; do
        cp "$SRC/$from" "$1/$name"
    done < <(awk '$1 == "file" { print $2, $3 }' "$PT_LICENSES")
}

# Fails unless the notices that ship name every statically linked component.
# ⚠️ The vendor directory is the list: add native/vendor/foo/ and this fails until
# THIRD-PARTY-NOTICES.md names foo.
check_notices() {   # the THIRD-PARTY-NOTICES.md inside the package
    local d c missing=""
    local components=()
    # ⚠️ `basename`, not `ls -1`: a trailing slash makes `dr_flac/` match a path in the notices' prose
    # instead of the component's name, and the check passes on nothing.
    for d in "$SRC"/native/vendor/*/; do components+=("$(basename "$d")"); done
    mapfile -t -O "${#components[@]}" components < <(awk '$1 == "notice" { print $2 }' "$PT_LICENSES")
    echo "licence notices (${#components[@]} components):"
    for c in "${components[@]}"; do
        if grep -qi -- "$c" "$1"; then
            echo "  ok      $c"
        else
            echo "  MISSING $c"
            missing="$missing $c"
        fi
    done
    if [ -n "$missing" ]; then
        echo "FAIL: statically linked but not named in THIRD-PARTY-NOTICES.md:$missing"
        echo "      The artifact is the only thing a user receives. Document it there, then rebuild."
        exit 1
    fi
}
