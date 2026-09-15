import Foundation

@MainActor
private final class FakePlayer: AudioPreviewPlaying {
    var onFinish: ((Bool) -> Void)?
    var playResult = true
    private(set) var playCount = 0
    private(set) var stopCount = 0

    func play() -> Bool { playCount += 1; return playResult }
    func stop() { stopCount += 1 }
    func finish(_ succeeded: Bool = true) { onFinish?(succeeded) }
}

@MainActor
private enum FakeError: Error { case failed }

@MainActor
private final class FakeFactory: AudioPreviewPlayerFactory {
    var failureURLs = Set<URL>()
    private(set) var requestedURLs: [URL] = []
    private(set) var players: [FakePlayer] = []

    func makePlayer(for url: URL) throws -> any AudioPreviewPlaying {
        requestedURLs.append(url)
        guard !failureURLs.contains(url) else { throw FakeError.failed }
        let player = FakePlayer()
        players.append(player)
        return player
    }
}

@MainActor
private func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
    guard condition() else { fatalError(message) }
}

@main
@MainActor
struct AudioPreviewControllerTests {
    static func main() {
        let a = URL(fileURLWithPath: "/tmp/A.wav")
        let b = URL(fileURLWithPath: "/tmp/B.wav")

        do {
            let factory = FakeFactory()
            let controller = AudioPreviewController(playerFactory: factory)
            controller.select(a)
            expect(controller.state.selectedURL == a, "A must become selected")
            controller.play()
            expect(controller.state.isPlaying, "A must play")
            expect(factory.players[0].playCount == 1, "play must reach A")
        }

        do {
            let factory = FakeFactory()
            let controller = AudioPreviewController(playerFactory: factory)
            controller.select(a)
            let old = factory.players[0]
            controller.play()
            controller.select(b)
            expect(old.stopCount == 1, "selecting B must stop A")
            expect(controller.state.selectedURL == b && !controller.state.isPlaying, "B must be idle")
            old.finish()
            expect(controller.state.selectedURL == b && !controller.state.isPlaying, "stale A completion must not affect B")
        }

        do {
            let factory = FakeFactory()
            let controller = AudioPreviewController(playerFactory: factory)
            controller.select(a)
            let player = factory.players[0]
            controller.stop()
            controller.stop()
            expect(player.stopCount == 1, "stop must be idempotent")
            controller.play()
            let playing = factory.players[1]
            player.finish()
            expect(controller.state.isPlaying, "old stopped completion must not stop restarted preview")
            playing.finish()
            expect(!controller.state.isPlaying, "completion must return to idle")
        }

        do {
            let factory = FakeFactory()
            factory.failureURLs.insert(a)
            let controller = AudioPreviewController(playerFactory: factory)
            controller.select(a)
            expect(!controller.state.isPlaying && controller.state.errorMessage != nil, "factory failure must be idle with error")
        }

        do {
            let factory = FakeFactory()
            let controller = AudioPreviewController(playerFactory: factory)
            controller.select(a)
            factory.players[0].playResult = false
            controller.play()
            expect(!controller.state.isPlaying && controller.state.errorMessage != nil, "play=false must be idle with error")
            expect(factory.players[0].stopCount == 1, "failed play must release the player")
        }

        print("AudioPreviewController tests passed")
    }
}
