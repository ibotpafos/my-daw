import AppKit
import Foundation
import Darwin

private func reportError(_ message: String) {
    FileHandle.standardError.write(Data((message + "\n").utf8))
}

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
        reportError("Expected --quit or --verify and an absolute bundle path.")
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
                reportError("The application refused a normal quit request; build cancelled.")
                return 1
            }
        }
        let deadline = Date().addingTimeInterval(15)
        while !matchingApplications().isEmpty && Date() < deadline {
            RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        }
        guard matchingApplications().isEmpty else {
            reportError("The application is still open. Save/close it before rebuilding; no forced termination was attempted.")
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
        reportError("The expected application did not finish launching.")
        return 1
    }
    return 0
}

exit(controlApplication())
