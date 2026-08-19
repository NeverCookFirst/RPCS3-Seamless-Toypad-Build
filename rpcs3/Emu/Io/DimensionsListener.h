#pragma once

// TCP listener that lets external companion apps (e.g. LegoToypad) drive the
// emulated LEGO Dimensions Toypad, wire-compatible with harrysof's
// Cemu-2.6-Remote-Toypad-Build listener:
//
//   LOAD:   0x01 pad index 0x00 0x00 + 180 tag bytes + u16le path length + path
//   REMOVE: 0x02 pad index 0x00 0x00
//   MOVE:   0x03 destPad destIndex srcPad srcIndex
//
// pad: 1 = center, 2 = left, 3 = right; index: 0..6.
// Listens on 127.0.0.1:9191 (override with env RPCS3_TOYPAD_PORT).

void dimensions_listener_start();
void dimensions_listener_stop();
