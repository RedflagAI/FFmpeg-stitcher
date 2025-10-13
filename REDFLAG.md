# Redflag specific info
This repo contains the Redflag AI fork of FFMPEG. It is mostly
identical to release 7.1, but has some patches for some additional
filters and slightly better mpegts compatability, notably not
clobbering SCTE markers.

## Building and Installing
This build targets `/usr/local` as a prefix. Note that FFMPEG requires
GNU make, BSD make will not work. Remember to point `PKG_CONFIG_PATH`
to include `/usr/local/lib/pkgconfig` if it doesn't not already
include it and you plan to use `pkg-config`.

To build this version of ffmpeg you need all of the standard ffmpeg
dependencies:
```
autoconf automake build-essential cmake git-core libass-dev libfreetype6-dev libgnutls28-dev libmp3lame-dev libtool libvorbis-dev meson ninja-build pkg-config texinfo wget yasm zlib1g-dev
```
along with
```
libx264-dev
```
Note that these are the ubuntu package names, the names may vary per
linux distribution.

### SRT
For SRT support in FFMPEG please also build libsrt.

Libsrt requires 
```
openssl
```
as a dependency.

Build and install it with
```
git clone https://github.com/haivision/srt
cd srt
cmake . 
cmake --build .
cmake --install . --prefix /usr/local
```

### FFMPEG 
In this directory build and install with
```
./configure --enable-nonfree --enable-gpl --enable-libsrt \
--enable-libx264 --disable-doc --enable-bsf=scte35ptsadjust \
--enable-pic --enable-static --enable-shared --prefix=/usr/local
make all -j
sudo make install
```
