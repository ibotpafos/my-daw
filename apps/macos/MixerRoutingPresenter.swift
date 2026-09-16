import AppKit

/// Retains one reusable non-modal routing window. It owns no project state: every
/// edit calls back into the same revision-aware Session commands as channel strips.
@MainActor
final class MixerRoutingPresenter: NSObject, NSWindowDelegate {
    static let shared = MixerRoutingPresenter()
    private var controller: NSWindowController?
    private var matrix: MixerRoutingMatrixView?
    private var stripsProvider: (() -> [MixerStripModel])?

    func present(strips:@escaping ()->[MixerStripModel], onOutput:@escaping (UInt64,UInt64)->Void, onSend:@escaping (UInt64,MixerSendAction)->Void) {
        let matrix: MixerRoutingMatrixView
        let window: NSWindow
        if let existing=self.matrix,let existingWindow=controller?.window {
            matrix=existing;window=existingWindow
        } else {
            matrix=MixerRoutingMatrixView(frame:NSRect(x:0,y:0,width:900,height:540))
            window=NSWindow(contentRect:NSRect(x:0,y:0,width:900,height:540),styleMask:[.titled,.closable,.resizable,.miniaturizable],backing:.buffered,defer:false)
            window.title="My DAW · Routing Matrix"
            window.minSize=NSSize(width:560,height:300)
            window.contentView=matrix
            window.isReleasedWhenClosed=false
            window.delegate=self
            controller=NSWindowController(window:window)
            self.matrix=matrix
        }
        stripsProvider=strips
        matrix.strips=strips()
        matrix.onOutput={ [weak self] id,bus in
            onOutput(id,bus)
            DispatchQueue.main.async { [weak self] in self?.reload() }
        }
        matrix.onSend={ [weak self] id,action in
            onSend(id,action)
            DispatchQueue.main.async { [weak self] in self?.reload() }
        }
        window.center()
        controller?.showWindow(nil)
        NSApp.activate(ignoringOtherApps:true)
        window.makeKeyAndOrderFront(nil)
    }

    func reload() { if let stripsProvider { matrix?.strips=stripsProvider() } }
    func close() { controller?.close();controller=nil;matrix=nil;stripsProvider=nil }
    func windowDidBecomeKey(_ notification:Notification){reload()}
    func windowWillClose(_ notification:Notification){controller=nil;matrix=nil;stripsProvider=nil}
}
