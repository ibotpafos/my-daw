import AVFoundation
import Foundation

@MainActor
protocol AudioPreviewPlaying: AnyObject {
    var onFinish: ((Bool) -> Void)? { get set }
    func play() -> Bool
    func stop()
}

@MainActor
protocol AudioPreviewPlayerFactory {
    func makePlayer(for url: URL) throws -> any AudioPreviewPlaying
}

@MainActor
private final class AVAudioPreviewPlayer: NSObject, AudioPreviewPlaying, @preconcurrency AVAudioPlayerDelegate {
    private let player: AVAudioPlayer
    var onFinish: ((Bool) -> Void)?

    init(url: URL) throws {
        player = try AVAudioPlayer(contentsOf: url)
        super.init()
        player.delegate = self
        player.prepareToPlay()
    }

    func play() -> Bool { player.play() }
    func stop() { player.stop() }

    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        onFinish?(flag)
    }
}

@MainActor
private struct SystemAudioPreviewPlayerFactory: AudioPreviewPlayerFactory {
    func makePlayer(for url: URL) throws -> any AudioPreviewPlaying {
        try AVAudioPreviewPlayer(url: url)
    }
}

/// Owns the short, local audition used by the browser. It deliberately has no
/// relationship to the DAW transport, so stopping or finishing a preview never
/// changes the project playhead.
@MainActor
final class AudioPreviewController {
    struct State: Equatable {
        var selectedURL: URL?
        var isPlaying: Bool
        var errorMessage: String?
    }

    private let playerFactory: any AudioPreviewPlayerFactory
    private var player: (any AudioPreviewPlaying)?
    private var generation = UInt64.zero

    private(set) var state = State(selectedURL: nil, isPlaying: false, errorMessage: nil) {
        didSet {
            guard oldValue != state else { return }
            onChange?(state)
        }
    }
    var onChange: ((State) -> Void)?

    init(playerFactory: any AudioPreviewPlayerFactory = SystemAudioPreviewPlayerFactory()) {
        self.playerFactory = playerFactory
    }

    func select(_ url: URL?) {
        generation &+= 1
        player?.onFinish = nil
        player?.stop()
        player = nil
        state = State(selectedURL: url, isPlaying: false, errorMessage: nil)

        guard url != nil else { return }
        loadPlayerIfNeeded()
    }

    func play() {
        guard state.selectedURL != nil else {
            state.isPlaying = false
            state.errorMessage = "Сначала выбери аудиофайл для предпрослушивания."
            return
        }
        guard loadPlayerIfNeeded(), let player else { return }
        guard player.play() else {
            player.onFinish = nil
            player.stop()
            self.player = nil
            state.isPlaying = false
            state.errorMessage = "Не удалось запустить предпрослушивание аудио."
            return
        }
        state.isPlaying = true
        state.errorMessage = nil
    }

    func stop() {
        guard player != nil || state.isPlaying else { return }
        player?.onFinish = nil
        player?.stop()
        player = nil
        state.isPlaying = false
    }

    @discardableResult
    private func loadPlayerIfNeeded() -> Bool {
        guard player == nil, let url = state.selectedURL else { return player != nil }
        let token = generation
        do {
            let newPlayer = try playerFactory.makePlayer(for: url)
            newPlayer.onFinish = { [weak self, weak newPlayer] _ in
                guard let self, token == self.generation else { return }
                guard let current = self.player, let newPlayer,
                      current === newPlayer else { return }
                self.player = nil
                self.state.isPlaying = false
            }
            player = newPlayer
            return true
        } catch {
            state.isPlaying = false
            state.errorMessage = "Не удалось открыть аудио для предпрослушивания."
            return false
        }
    }
}
