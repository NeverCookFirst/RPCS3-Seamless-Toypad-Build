#pragma once

// TCP listener that lets external companion apps (e.g. LegoToypad) drive the
// emulated LEGO Dimensions Toypad, wire-compatible with the Cemu and shadPS4
// listeners (LED protocol version 2):
//
//   LOAD:   0x01 pad index 0x00 0x00 + 180 tag bytes + u16le path length + path
//   REMOVE: 0x02 pad index 0x00 0x00
//   MOVE:   0x03 destPad destIndex srcPad srcIndex
//   GET_LED:0x04 0x00 0x00 0x00 0x00 -> 40-byte LED snapshot:
//           { 'L', serial, version(2), region count(3), then 3 regions x 12
//             bytes: pad, mode, r, g, b, from_r, from_g, from_b, on_ms, off_ms,
//             count, speed_ms }
//
// pad: 1 = center, 2 = left, 3 = right; index: 0..6.
// Listens on 127.0.0.1:9191 (override with env RPCS3_TOYPAD_PORT).

void dimensions_listener_start();
void dimensions_listener_stop();
