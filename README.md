# musicwidget

A music widget written in C because I thought it'd be funny.

Displays currently playing media on a Wayland compositor that supports wlr-layer-shell.

## Dependencies

- wayland-client
- cairo
- wayland-cursor

## Build

gcc -o musicwidget musicwidget.c \
  wlr-layer-shell-unstable-v1-client-protocol.c \
  xdg-shell-client-protocol.c \
  $(pkg-config --cflags --libs wayland-client cairo) \
  -lwayland-cursor -lm -lrt

## Install

cp musicwidget ~/.local/bin/
