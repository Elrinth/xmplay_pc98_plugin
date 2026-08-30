#!/bin/sh
# Download Hoot XML-only catalogue (no game music).
# Official / Kurohane packs move; we try a few URLs then tell the user.
set -e
DEST="${1:-dist/hoot-xml}"
mkdir -p "$DEST"
URLS="
https://kurohane.net/hoot/xml.zip
http://dmpsoft.s17.xrea.com/hoot/xml.zip
"
ok=0
for u in $URLS; do
	echo "fetch-xml: trying $u"
	if command -v curl >/dev/null 2>&1; then
		if curl -fsSL -o "$DEST/xml.zip" "$u"; then
			ok=1
			break
		fi
	elif command -v wget >/dev/null 2>&1; then
		if wget -q -O "$DEST/xml.zip" "$u"; then
			ok=1
			break
		fi
	fi
done
if [ "$ok" != 1 ]; then
	echo "fetch-xml: no pack downloaded."
	echo "Get the XML-only Hoot catalogue from"
	echo "  http://dmpsoft.s17.xrea.com/hoot/"
	echo "  https://kurohane.net/hoot/"
	echo "and unpack hoot.xml + gamelists into $DEST"
	echo "(or set Config → Hoot XML to that folder)."
	exit 0
fi
if command -v unzip >/dev/null 2>&1; then
	unzip -o -q "$DEST/xml.zip" -d "$DEST" || true
fi
echo "fetch-xml: unpacked into $DEST"
