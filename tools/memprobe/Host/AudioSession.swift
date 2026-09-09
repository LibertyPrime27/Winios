import AVFoundation

/// The app's audio session, activated before a guest runs.
///
/// Playback, not the default: a game's sound is the point of the game, so it
/// has to play with the ringer switch on silent, and keep playing under the
/// lock screen for as long as the guest runs. mixWithOthers so a podcast the
/// user had going is ducked rather than killed -- their choice to stop it.
enum AudioSession {
    static func activate() {
        let s = AVAudioSession.sharedInstance()
        try? s.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try? s.setActive(true)
    }
}
