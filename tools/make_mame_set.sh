#!/bin/sh
# Build a MAME-compatible software-list set for DOOM ON RAILS.
# Output: dist/mame/roms/neogeo/doomrails.zip + dist/mame/hash/neogeo.xml + the ngdevkit open BIOS.
# Run from the repo root after a full cart build (neogeo/build/rom populated).
set -e
R=neogeo/build/rom
D=dist/mame
mkdir -p $D/hash $D/roms/neogeo
rm -f $D/roms/neogeo/doomrails.zip
( cd $R && zip -q ../../../$D/roms/neogeo/doomrails.zip 202-p1.p1 202-p2.p2 202-s1.s1 202-m1.m1 202-v1.v1 202-c1.c1 202-c2.c2 )
cp $R/neogeo.zip $D/roms/neogeo.zip 2>/dev/null || cp $R/neogeo.zip $D/roms/ 2>/dev/null || true
cp $R/neogeo.zip $D/roms/neogeo/../neogeo.zip 2>/dev/null || true
sz(){ stat -c %s "$R/$1" 2>/dev/null || stat -f %z "$R/$1"; }   # GNU stat (Linux) first, BSD/macOS fallback
hx(){ printf '0x%x' "$1"; }
crc(){ python3 -c "import zlib,sys;print('%08x'%(zlib.crc32(open(sys.argv[1],'rb').read())&0xffffffff))" "$R/$1"; }
sha(){ if command -v sha1sum >/dev/null 2>&1; then sha1sum "$R/$1" | cut -d' ' -f1; else shasum -a 1 "$R/$1" | cut -d' ' -f1; fi; }   # Linux coreutils sha1sum first, macOS shasum fallback (NOT a pipeline-|| -- that masks a missing tool as an empty hash)
P1=$(sz 202-p1.p1); P2=$(sz 202-p2.p2); S1=$(sz 202-s1.s1); M1=$(sz 202-m1.m1); V1=$(sz 202-v1.v1); C1=$(sz 202-c1.c1)
cat > $D/hash/neogeo.xml <<XML
<?xml version="1.0"?>
<!DOCTYPE softwarelist SYSTEM "softwarelist.dtd">
<softwarelist name="neogeo" description="SNK Neo-Geo cartridges (DOOM ON RAILS homebrew overlay)">
	<software name="doomrails">
		<description>DOOM ON RAILS (homebrew tech demo)</description>
		<year>2026</year>
		<publisher>homebrew</publisher>
		<sharedfeat name="release" value="MVS,AES" />
		<sharedfeat name="compatibility" value="MVS,AES" />
		<part name="cart" interface="neo_cart">
			<dataarea name="maincpu" width="16" endianness="big" size="$(hx $((P1+P2)))">
				<rom loadflag="load16_word_swap" name="202-p1.p1" offset="0x000000" size="$(hx $P1)" crc="$(crc 202-p1.p1)" sha1="$(sha 202-p1.p1)" />
				<rom loadflag="load16_word_swap" name="202-p2.p2" offset="0x100000" size="$(hx $P2)" crc="$(crc 202-p2.p2)" sha1="$(sha 202-p2.p2)" />
			</dataarea>
			<dataarea name="fixed" size="$(hx $S1)">
				<rom offset="0x000000" size="$(hx $S1)" name="202-s1.s1" crc="$(crc 202-s1.s1)" sha1="$(sha 202-s1.s1)" />
			</dataarea>
			<dataarea name="audiocpu" size="$(hx $M1)">
				<rom offset="0x000000" size="$(hx $M1)" name="202-m1.m1" crc="$(crc 202-m1.m1)" sha1="$(sha 202-m1.m1)" />
			</dataarea>
			<dataarea name="ymsnd:adpcma" size="$(hx $V1)">
				<rom name="202-v1.v1" offset="0x000000" size="$(hx $V1)" crc="$(crc 202-v1.v1)" sha1="$(sha 202-v1.v1)" />
			</dataarea>
			<dataarea name="ymsnd:adpcmb" size="$(hx $V1)">
				<rom name="202-v1.v1" offset="0x000000" size="$(hx $V1)" crc="$(crc 202-v1.v1)" sha1="$(sha 202-v1.v1)" />
			</dataarea>
			<dataarea name="sprites" size="$(hx $((C1*2)))">
				<rom loadflag="load16_byte" name="202-c1.c1" offset="0x000000" size="$(hx $C1)" crc="$(crc 202-c1.c1)" sha1="$(sha 202-c1.c1)" />
				<rom loadflag="load16_byte" name="202-c2.c2" offset="0x000001" size="$(hx $C1)" crc="$(crc 202-c2.c2)" sha1="$(sha 202-c2.c2)" />
			</dataarea>
		</part>
	</software>
</softwarelist>
XML
echo "dist/mame ready: roms/neogeo/doomrails.zip + hash/neogeo.xml"
