// MidiStatus.swift — detect the Eleven Rack's CoreMIDI ports for the panel's MIDI
// indicator. macOS's built-in USB-MIDI driver exposes the device on its own as the
// "Eleven Rack Rig" / "Eleven Rack External" ports; we just look for any endpoint
// whose name contains "Eleven Rack".
//
// A process must hold a CoreMIDI client to get a live connection to the MIDI
// server and reliably enumerate endpoints — a background/menu-bar (LSUIElement)
// app launched by launchd otherwise sees zero endpoints. So we create one client
// once, up front, before querying.

import Foundation
import CoreMIDI

enum MidiStatus {
    /// Persistent MIDI-server connection for this process (created once).
    private static let client: MIDIClientRef = {
        var c = MIDIClientRef()
        MIDIClientCreate("Eleven Rack (status)" as CFString, nil, nil, &c)
        return c
    }()

    /// True if a MIDI source or destination whose name contains "Eleven Rack" exists.
    static var elevenRackPresent: Bool {
        _ = client   // force the client (and thus the server connection) into existence
        return endpointNames(count: MIDIGetNumberOfSources(), get: MIDIGetSource).contains { $0.contains(ER.deviceName) }
            || endpointNames(count: MIDIGetNumberOfDestinations(), get: MIDIGetDestination).contains { $0.contains(ER.deviceName) }
    }

    private static func endpointNames(count: Int, get: (Int) -> MIDIEndpointRef) -> [String] {
        (0..<count).compactMap { i in
            let ep = get(i)
            var cf: Unmanaged<CFString>?
            guard MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cf) == noErr,
                  let name = cf?.takeRetainedValue() as String? else { return nil }
            return name
        }
    }
}
