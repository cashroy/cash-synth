#!/bin/bash
# Builds the macOS installer: CashSynth-mac.pkg with three choices
# (VST3, AU, Standalone app), all on by default. Bundles are ad-hoc signed; the pkg
# itself is unsigned (no Developer ID), so macOS asks before opening it.
#
#   packaging/mac/build_pkg.sh <artefacts dir> <version> <output dir>
#   e.g. packaging/mac/build_pkg.sh build-release/CashSynth_artefacts/Release 1.0.0 dist
set -euo pipefail
export COPYFILE_DISABLE=1 # no ._ AppleDouble files in the payload

ART="$1"
VERSION="$2"
OUT="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ID=com.cashroy.cashsynth
NAME="Cash Synth"

for b in "$ART/VST3/$NAME.vst3" "$ART/AU/$NAME.component" "$ART/Standalone/$NAME.app"; do
    [ -d "$b" ] || { echo "missing $b" >&2; exit 1; }
    [ -n "$(ls "$b/Contents/MacOS" 2>/dev/null)" ] || { echo "no binary in $b (not built?)" >&2; exit 1; }
done

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT" "$WORK/pkgs" "$WORK/res" "$WORK/scripts-au"

# One component package per format. Not relocatable: always install to the standard folder.
component () { # <bundle> <install location> <pkg id> <pkg file> [scripts dir]
    local stage="$WORK/stage-$3"
    mkdir -p "$stage"
    ditto --noextattr --noqtn "$1" "$stage/$(basename "$1")"
    codesign --force --deep --sign - "$stage/$(basename "$1")" 2>/dev/null
    xattr -cr "$stage" 2>/dev/null || true # no extended attributes (they'd ship as ._ files)
    pkgbuild --analyze --root "$stage" "$WORK/$3.plist" >/dev/null
    plutil -replace 0.BundleIsRelocatable -bool NO "$WORK/$3.plist"
    local extra=()
    [ -n "${5:-}" ] && extra=(--scripts "$5")
    pkgbuild --root "$stage" --component-plist "$WORK/$3.plist" --identifier "$3" --version "$VERSION" \
             --install-location "$2" ${extra[@]+"${extra[@]}"} "$WORK/pkgs/$4" >/dev/null
}

# The AU: make the system re-scan Audio Units after installing.
cat > "$WORK/scripts-au/postinstall" <<'SH'
#!/bin/sh
killall -9 AudioComponentRegistrar 2>/dev/null || true
exit 0
SH
chmod +x "$WORK/scripts-au/postinstall"

component "$ART/VST3/$NAME.vst3"      "/Library/Audio/Plug-Ins/VST3"       "$ID.vst3" vst3.pkg
component "$ART/AU/$NAME.component"   "/Library/Audio/Plug-Ins/Components" "$ID.au"   au.pkg "$WORK/scripts-au"
component "$ART/Standalone/$NAME.app" "/Applications"                      "$ID.app"  app.pkg

cp "$ROOT/LICENSE" "$WORK/res/LICENSE.txt"
cat > "$WORK/res/welcome.html" <<HTML
<html><body style="font-family: -apple-system, Helvetica; font-size: 13px">
<h2>Cash Synth $VERSION</h2>
<p>A money-themed synthesizer by <b>Cashroy</b>: a \$100 bill on the front, every knob on the back,
and Cash Match to rebuild any sample as a playable synth sound.</p>
<p>This installs the <b>VST3</b> and <b>Audio Unit</b> plug-ins (for all users) and the
<b>Standalone</b> app. Choose <i>Customise</i> to leave any of them out.</p>
<p>After installing, rescan plug-ins in your DAW (Logic does this on its next launch).</p>
</body></html>
HTML

cat > "$WORK/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Cash Synth $VERSION</title>
    <organization>com.cashroy</organization>
    <welcome file="welcome.html"/>
    <license file="LICENSE.txt"/>
    <options customize="allow" require-scripts="false" hostArchitectures="x86_64,arm64"/>
    <os-version min="11.0"/>
    <domains enable_localSystem="true"/>
    <choices-outline>
        <line choice="vst3"/>
        <line choice="au"/>
        <line choice="app"/>
    </choices-outline>
    <choice id="vst3" title="VST3 plug-in" description="Cash Synth.vst3 in /Library/Audio/Plug-Ins/VST3">
        <pkg-ref id="$ID.vst3"/>
    </choice>
    <choice id="au" title="Audio Unit plug-in" description="Cash Synth.component in /Library/Audio/Plug-Ins/Components (Logic, GarageBand)">
        <pkg-ref id="$ID.au"/>
    </choice>
    <choice id="app" title="Standalone app" description="Cash Synth.app in /Applications">
        <pkg-ref id="$ID.app"/>
    </choice>
    <pkg-ref id="$ID.vst3" version="$VERSION">vst3.pkg</pkg-ref>
    <pkg-ref id="$ID.au" version="$VERSION">au.pkg</pkg-ref>
    <pkg-ref id="$ID.app" version="$VERSION">app.pkg</pkg-ref>
</installer-gui-script>
XML

PKG="$OUT/CashSynth-mac.pkg" # fixed name: the website links releases/latest/download/CashSynth-mac.pkg
productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK/pkgs" --resources "$WORK/res" "$PKG"
echo "$PKG"
