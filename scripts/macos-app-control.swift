import AppKit
import Foundation
import Darwin

// A development helper, not an application source. Resolve the exact bundle,
// and let NSRunningApplication request normal termination (including save UI).
@MainActor
func controlApplication() -> Int32 {
    let arguments = CommandLine.arguments
    if arguments.count == 2 && arguments[1] == "--help" {
        print("usage: macos-app-control.swift [--quit|--verify] /absolute/path/App.app")
        return 0
    }
    guard arguments.count == 3, ["--quit", "--verify"].contains(arguments[1]),
          arguments[2].hasPrefix("/") else {
        fputs("Expected --quit or --verify and an absolute bundle path.\n", stderr)
        return 2
    }
    let target = URL(fileURLWithPath: arguments[2]).standardizedFileURL.resolvingSymlinksInPath()
    func matchingApplications() -> [NSRunningApplication] {
        NSWorkspace.shared.runningApplications.filter {
            $0.bundleURL?.standardizedFileURL.resolvingSymlinksInPath() == target && !$0.isTerminated
        }
    }
    if arguments[1] == "--quit" {
        let applications = matchingApplications()
        for application in applications {
            if !application.terminate() && !application.isTerminated {
                fputs("The application refused a normal quit request; build cancelled.\n", stderr)
                return 1
            }
        }
        let deadline = Date().addingTimeInterval(15)
        while !matchingApplications().isEmpty && Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        }
        guard matchingApplications().isEmpty else {
            fputs("The application is still open. Save/close it before rebuilding; no forced termination was attempted.\n", stderr)
            return 1
        }
    } else {
        let deadline = Date().addingTimeInterval(5)
        repeat {
            if let application = matchingApplications().first(where: { $0.isFinishedLaunching }) {
                print("VERIFY: exact bundle is running, pid=\(application.processIdentifier), path=\(target.path)")
                return 0
            }
            RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        } while Date() < deadline
        fputs("The expected application did not finish launching.\n", stderr)
        return 1
    }
    return 0
}

exit(controlApplication())
