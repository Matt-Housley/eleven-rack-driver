// CoreAudioDevice.swift — locate the "Eleven Rack" HAL device and read/set its
// nominal sample rate. Setting the rate here is exactly what Audio MIDI Setup
// does; the engine's rate-follow then retunes the hardware clock to match. This
// avoids adding any new control channel to the engine.

import Foundation
import CoreAudio

enum CoreAudioDevice {
    /// Find the AudioDeviceID whose name is the Eleven Rack, or nil.
    static func find() -> AudioDeviceID? {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)

        var dataSize: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &dataSize) == noErr else { return nil }

        let count = Int(dataSize) / MemoryLayout<AudioDeviceID>.size
        guard count > 0 else { return nil }
        var ids = [AudioDeviceID](repeating: 0, count: count)
        guard AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &dataSize, &ids) == noErr else { return nil }

        for id in ids where name(of: id) == ER.deviceName { return id }
        return nil
    }

    /// Device name, or nil.
    static func name(of id: AudioDeviceID) -> String? {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioObjectPropertyName,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var name: CFString = "" as CFString
        var size = UInt32(MemoryLayout<CFString>.size)
        let status = withUnsafeMutablePointer(to: &name) {
            AudioObjectGetPropertyData(id, &addr, 0, nil, &size, $0)
        }
        return status == noErr ? (name as String) : nil
    }

    /// Current nominal sample rate in Hz, or nil.
    static func currentRate(of id: AudioDeviceID) -> UInt32? {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var rate: Float64 = 0
        var size = UInt32(MemoryLayout<Float64>.size)
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &rate) == noErr else { return nil }
        return UInt32(rate.rounded())
    }

    /// Set the device's nominal sample rate. Returns true on success.
    @discardableResult
    static func setRate(_ hz: UInt32, on id: AudioDeviceID) -> Bool {
        var addr = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyNominalSampleRate,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var rate = Float64(hz)
        let size = UInt32(MemoryLayout<Float64>.size)
        return AudioObjectSetPropertyData(id, &addr, 0, nil, size, &rate) == noErr
    }
}
