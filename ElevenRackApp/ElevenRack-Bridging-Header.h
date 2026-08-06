/**
 * @file ElevenRack-Bridging-Header.h
 * @brief Clang bridging header exposing the shared-memory ring to Swift.
 *
 * Passed to @c swiftc via @c -import-objc-header. It surfaces the ring struct,
 * @c er_ring_attach / @c er_ring_close, and the read-only metering peek helpers
 * so the menu-bar app can observe engine status and per-channel levels without
 * disturbing the producer/consumer indices.
 */
#import "ERAudioRing.h"
