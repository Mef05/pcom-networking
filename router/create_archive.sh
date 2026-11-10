#!/bin/bash

if [ ! -f README.md ]; then
	echo "No README.md present! Exiting..." >&2
	exit 1
fi

if [ ! -f Makefile ]; then
	echo "No Makefile present! Exiting..." >&2
	exit 1
fi

rm -rf archive.zip
sudo make clean
zip -r archive.zip lib/ include/ *.cpp Makefile README.md
if [ -f arp_table.txt ]; then
	zip archive.zip arp_table.txt
fi
for f in subiect1.png subiect3.png subiect4.png; do
	if [ -f "$f" ]; then
		zip archive.zip "$f"
	fi
done
