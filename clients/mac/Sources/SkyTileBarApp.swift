import AppKit
import Carbon.HIToolbox
import SwiftUI

// Menu bar app: a checklist icon with the open count. Clicking it (or Cmd+Option+T from
// anywhere) opens a Liquid Glass panel with the list.
@main
struct SkyTileBarApp: App {
    @StateObject private var store = TodoStore()
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate

    var body: some Scene {
        MenuBarExtra {
            TodoPanel()
                .environmentObject(store)
                .onAppear { store.start() }
        } label: {
            let n = store.open.count
            Image(systemName: store.error != nil ? "exclamationmark.circle" : n > 0 ? "checklist" : "checklist.checked")
            if n > 0 { Text("\(n)") }
        }
        .menuBarExtraStyle(.window)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private var hotKey: EventHotKeyRef?

    func applicationDidFinishLaunching(_ notification: Notification) {
        registerHotKey()
        // `open SkyTile.app --args --show` opens the panel on launch (for screenshots).
        if CommandLine.arguments.contains("--show") {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { AppDelegate.togglePanel() }
        }
        // `--snapshot <png>`: also write the open panel to a PNG (layout check only: it
        // draws without the live glass behind it).
        if let i = CommandLine.arguments.firstIndex(of: "--snapshot"), i + 1 < CommandLine.arguments.count {
            let path = CommandLine.arguments[i + 1]
            DispatchQueue.main.asyncAfter(deadline: .now() + 4) { AppDelegate.snapshot(to: path) }
        }
    }

    private static func snapshot(to path: String) {
        guard let view = NSApp.windows.first(where: { $0.isVisible && !$0.className.contains("StatusBarWindow") })?.contentView,
              let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
    }

    // Cmd+Option+T: a Carbon hot key, which needs no Accessibility permission.
    private func registerHotKey() {
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, _, _ in
            DispatchQueue.main.async { AppDelegate.togglePanel() }
            return noErr
        }, 1, &spec, nil, nil)
        let id = EventHotKeyID(signature: OSType(0x534B_5954), id: 1) // "SKYT"
        RegisterEventHotKey(UInt32(kVK_ANSI_T), UInt32(cmdKey | optionKey), id, GetApplicationEventTarget(), 0, &hotKey)
    }

    // MenuBarExtra has no API to open it, so click its status item button.
    static func togglePanel() {
        NSApp.activate()
        for window in NSApp.windows where window.className.contains("StatusBarWindow") {
            if let button = findButton(in: window.contentView) {
                button.performClick(nil)
                return
            }
        }
    }

    private static func findButton(in view: NSView?) -> NSButton? {
        guard let view else { return nil }
        if let b = view as? NSButton { return b }
        for sub in view.subviews { if let b = findButton(in: sub) { return b } }
        return nil
    }
}
