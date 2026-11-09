// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The system's side of the iOS and macOS apps (docs/hosts/apple.md), for src/gui/apple.m: the document picker and the share
// sheet SDL lacks on iOS. The menu and the runtime are SDL's window; this only presents over it.
import Foundation

#if os(iOS)
import UIKit
import UniformTypeIdentifiers

/// apple.m's callback: the path picked, nil for none
public typealias SNXPicked = @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?) -> Void
/// apple.m's log, a line for each step of the picker: nil logs none
public typealias SNXPickLog = @convention(c) (UnsafePointer<CChar>) -> Void

/// The view controller on top of the window's: one presenting another can't present a third
private func presenter(_ window: UnsafeMutableRawPointer) -> UIViewController? {
    var top = Unmanaged<UIWindow>.fromOpaque(window).takeUnretainedValue().rootViewController
    while let shown = top?.presentedViewController { top = shown }
    return top
}

/// The picker's delegate, kept while it shows: UIKit holds its delegate weakly
private final class Picker: NSObject, UIDocumentPickerDelegate {
    static var showing: Picker?
    let done: SNXPicked
    let data: UnsafeMutableRawPointer?
    let log: SNXPickLog?
    let opened = Date()

    init(done: SNXPicked, data: UnsafeMutableRawPointer?, log: SNXPickLog?) {
        self.done = done
        self.data = data
        self.log = log
    }

    func say(_ line: @autoclosure () -> String) {
        if let log { ("picker: " + line()).withCString { log($0) } }
    }

    private var waited: String { String(format: "%.1f s", -opened.timeIntervalSinceNow) }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        // a dump is read where it is, by its path, all the app's run: its scope stays open
        guard let url = urls.first else {
            say("an answer with no item, after \(waited)")
            return finish(nil)
        }
        let scoped = url.startAccessingSecurityScopedResource()
        // the home folder as ~, as snx_home_short gives the C side's paths: it names the app's container
        let shown = (url.resolvingSymlinksInPath().path as NSString).abbreviatingWithTildeInPath
        say("picked \(urls.count) after \(waited): \(shown), security scope \(scoped ? "open" : "refused")")
        say(describe(url))
        finish(url.path)
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        say("cancelled after \(waited)")
        finish(nil)
    }

    /// what the file system says of the item: a folder or not, its size, and iCloud's state of it
    private func describe(_ url: URL) -> String {
        let keys: Set<URLResourceKey> = [.isDirectoryKey, .fileSizeKey, .isReadableKey, .isUbiquitousItemKey,
                                         .ubiquitousItemDownloadingStatusKey, .volumeNameKey]
        guard let v = try? url.resourceValues(forKeys: keys) else { return "the item's values cannot be read" }
        func text<T>(_ value: T?) -> String { value.map { "\($0)" } ?? "?" }
        return "folder \(text(v.isDirectory)), size \(text(v.fileSize)), readable \(text(v.isReadable)), "
            + "iCloud \(text(v.isUbiquitousItem)) \(text(v.ubiquitousItemDownloadingStatus?.rawValue)), "
            + "volume \(text(v.volumeName))"
    }

    private func finish(_ path: String?) {
        if let path {
            path.withCString { done(data, $0) }
        } else {
            done(data, nil)
        }
        Picker.showing = nil
    }
}

@_cdecl("snx_apple_pick")
public func snxApplePick(_ window: UnsafeMutableRawPointer, _ folder: Bool, _ done: SNXPicked,
                         _ data: UnsafeMutableRawPointer?, _ log: SNXPickLog?) -> Bool {
    let delegate = Picker(done: done, data: data, log: log)
    guard Picker.showing == nil else {
        delegate.say("not opened: one shows already")
        return false
    }
    guard let top = presenter(window) else {
        delegate.say("not opened: the window has no view controller to show it over")
        return false
    }
    // opened in place, not copied: a dump is many gigabytes
    let picker = UIDocumentPickerViewController(forOpeningContentTypes: folder ? [.folder] : [.item], asCopy: false)
    picker.delegate = delegate
    picker.allowsMultipleSelection = false
    Picker.showing = delegate
    // an open with no answer after it is a pick that the system did not end
    delegate.say("opens for a \(folder ? "folder" : "file") over \(type(of: top))")
    top.present(picker, animated: true) { delegate.say("shown") }
    return true
}

/// The system's share sheet for one file (Share Log's): AirDrop, Messages, Mail, Save to Files. An iPad shows it as a
/// popover, which needs a place to point at: the middle of the window
@_cdecl("snx_apple_share")
public func snxAppleShare(_ window: UnsafeMutableRawPointer, _ path: UnsafePointer<CChar>) -> Bool {
    // UIKit presents nothing, with no error, over a view controller that comes or goes (a second tap as a sheet closes)
    guard let top = presenter(window), !top.isBeingDismissed, !top.isBeingPresented else { return false }
    let sheet = UIActivityViewController(activityItems: [URL(fileURLWithPath: String(cString: path))],
                                         applicationActivities: nil)
    if let popover = sheet.popoverPresentationController, let view = top.view {
        popover.sourceView = view
        popover.sourceRect = CGRect(x: view.bounds.midX, y: view.bounds.midY, width: 0, height: 0)
        popover.permittedArrowDirections = []
    }
    top.present(sheet, animated: true)
    return true
}

#elseif os(macOS)

/// SDL has macOS's own file and folder dialogs: apple.m asks for none here
@_cdecl("snx_apple_pick")
public func snxApplePick(_ window: UnsafeMutableRawPointer, _ folder: Bool,
                         _ done: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?) -> Void,
                         _ data: UnsafeMutableRawPointer?,
                         _ log: (@convention(c) (UnsafePointer<CChar>) -> Void)?) -> Bool {
    false
}

/// Finder shares a file from its folder, which Share Log opens: apple.m asks for no sheet here
@_cdecl("snx_apple_share")
public func snxAppleShare(_ window: UnsafeMutableRawPointer, _ path: UnsafePointer<CChar>) -> Bool {
    false
}

#endif
