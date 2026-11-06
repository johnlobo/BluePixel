import AppKit
import WebKit

/// One entry of the CHM table of contents (WinAPE.hhc).
final class HelpTopic {
    let name: String, page: String?
    var children: [HelpTopic] = []
    init(name: String, page: String?) { self.name = name; self.page = page }
}

/// WinAPE help viewer: contents tree + web view over the pages extracted from WinAPE.chm.
final class HelpController: NSObject, NSOutlineViewDataSource, NSOutlineViewDelegate, WKNavigationDelegate {
    static var folder: URL { AppPaths.support.appendingPathComponent("Help") }

    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 960, height: 640), styleMask: [.titled, .closable, .resizable, .miniaturizable], backing: .buffered, defer: false)
    private let outline = NSOutlineView()
    private let web = WKWebView()
    private var roots: [HelpTopic] = []

    /// Returns nil (after telling the user) when the help pages are not available and cannot be extracted.
    static func make(winapeFolder: URL) -> HelpController? {
        if !FileManager.default.fileExists(atPath: folder.appendingPathComponent("WinAPE.hhc").path) { extract(winapeFolder) }
        guard FileManager.default.fileExists(atPath: folder.appendingPathComponent("WinAPE.hhc").path) else {
            let a = NSAlert(); a.messageText = "WinAPE help not found"
            a.informativeText = "BluePixel shows the help from WinAPE.chm. Install chmlib (brew install chmlib) so it can be extracted automatically, or extract WinAPE.chm into:\n\(folder.path)"
            a.runModal(); return nil
        }
        return HelpController()
    }

    /// Extracts WinAPE.chm with chmlib's extract_chmLib, if both can be found.
    private static func extract(_ winapeFolder: URL) {
        let chms = [winapeFolder.appendingPathComponent("Help/WinAPE.chm"), URL(fileURLWithPath: NSHomeDirectory() + "/Documents/WinAPE20B2/Help/WinAPE.chm")]
        guard let chm = chms.first(where: { FileManager.default.fileExists(atPath: $0.path) }),
              let tool = ["/opt/homebrew/bin/extract_chmLib", "/usr/local/bin/extract_chmLib"].first(where: { FileManager.default.isExecutableFile(atPath: $0) }) else { return }
        let p = Process(); p.executableURL = URL(fileURLWithPath: tool); p.arguments = [chm.path, folder.path]
        p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
        try? p.run(); p.waitUntilExit()
    }

    private override init() {
        super.init()
        roots = HelpController.parseContents((try? String(contentsOf: HelpController.folder.appendingPathComponent("WinAPE.hhc"), encoding: .isoLatin1)) ?? "")
        window.title = "BluePixel Help (WinAPE manual)"; window.isReleasedWhenClosed = false; window.center()
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("t")); outline.addTableColumn(col); outline.outlineTableColumn = col
        outline.headerView = nil; outline.dataSource = self; outline.delegate = self; outline.style = .sourceList
        let scroll = NSScrollView(); scroll.documentView = outline; scroll.hasVerticalScroller = true
        let split = NSSplitView(); split.isVertical = true; split.dividerStyle = .thin
        split.addArrangedSubview(scroll); split.addArrangedSubview(web)
        scroll.widthAnchor.constraint(greaterThanOrEqualToConstant: 220).isActive = true
        web.navigationDelegate = self
        window.contentView = split
        outline.reloadData(); roots.forEach { outline.expandItem($0) }
        split.layoutSubtreeIfNeeded(); split.setPosition(260, ofDividerAt: 0)
    }

    func show(page: String = "general.html") {
        open(page); window.makeKeyAndOrderFront(nil)
    }
    private func open(_ page: String) {
        let u = HelpController.folder.appendingPathComponent(page)
        web.loadFileURL(u, allowingReadAccessTo: HelpController.folder)
    }

    /// Parses the nested <UL><LI><OBJECT> sitemap of a .hhc file.
    static func parseContents(_ s: String) -> [HelpTopic] {
        var stack: [[HelpTopic]] = [[]]
        var lastAtLevel: [HelpTopic?] = [nil]
        let scanner = Scanner(string: s); scanner.charactersToBeSkipped = nil; scanner.caseSensitive = false
        func param(_ obj: String, _ name: String) -> String? {
            guard let r = obj.range(of: "name=\"\(name)\" value=\"", options: .caseInsensitive) else { return nil }
            let rest = obj[r.upperBound...]; return rest.firstIndex(of: "\"").map { String(rest[..<$0]) }
        }
        var depth = 0
        while !scanner.isAtEnd {
            _ = scanner.scanUpToString("<")
            guard scanner.scanString("<") != nil, let tag = scanner.scanUpToString(">") else { break }
            _ = scanner.scanString(">")
            let t = tag.lowercased()
            if t.hasPrefix("ul") { depth += 1; if depth > 1 { stack.append([]); lastAtLevel.append(nil) } }
            else if t.hasPrefix("/ul") {
                if depth > 1, let kids = stack.popLast() { lastAtLevel.removeLast(); lastAtLevel.last??.children = kids }
                depth -= 1
            } else if t.hasPrefix("object") && t.contains("text/sitemap") {
                let body = scanner.scanUpToString("</OBJECT>") ?? scanner.scanUpToString("</object>") ?? ""
                if let name = param(body, "Name") {
                    let topic = HelpTopic(name: name, page: param(body, "Local"))
                    stack[stack.count - 1].append(topic); lastAtLevel[lastAtLevel.count - 1] = topic
                }
            }
        }
        return stack.first ?? []
    }

    // MARK: outline
    func outlineView(_ o: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int { (item as? HelpTopic)?.children.count ?? roots.count }
    func outlineView(_ o: NSOutlineView, child index: Int, ofItem item: Any?) -> Any { (item as? HelpTopic)?.children[index] ?? roots[index] }
    func outlineView(_ o: NSOutlineView, isItemExpandable item: Any) -> Bool { !((item as? HelpTopic)?.children.isEmpty ?? true) }
    func outlineView(_ o: NSOutlineView, viewFor col: NSTableColumn?, item: Any) -> NSView? {
        let tf = makeLabel((item as? HelpTopic)?.name ?? ""); tf.lineBreakMode = .byTruncatingTail; return tf
    }
    func outlineViewSelectionDidChange(_ n: Notification) {
        if let t = outline.item(atRow: outline.selectedRow) as? HelpTopic, let p = t.page { open(p) }
    }

    // External links go to the browser; local pages stay here.
    func webView(_ w: WKWebView, decidePolicyFor a: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if let u = a.request.url, !u.isFileURL, a.navigationType == .linkActivated { NSWorkspace.shared.open(u); decisionHandler(.cancel) } else { decisionHandler(.allow) }
    }
}
