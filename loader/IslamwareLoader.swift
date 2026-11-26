import SwiftUI
import AppKit
import UniformTypeIdentifiers

enum LoaderState: Equatable {
    case needExe
    case readyToInstall
    case installing
    case installed
    case launching
}

struct RequirementItem: Identifiable, Equatable {
    let id: String
    let title: String
    let command: String
    var installed: Bool
}

final class LoaderViewModel: ObservableObject {
    @Published var state: LoaderState = .needExe
    @Published var statusNote: String = ""
    @Published var isHoveringDrop: Bool = false
    @Published var requirements: [RequirementItem] = [
        RequirementItem(
            id: "brew",
            title: "Homebrew",
            command: "/bin/bash -c \"$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"",
            installed: false
        ),
        RequirementItem(
            id: "rosetta",
            title: "Rosetta 2",
            command: "softwareupdate --install-rosetta --agree-to-license",
            installed: false
        ),
        RequirementItem(
            id: "wine",
            title: "Wine",
            command: "brew tap gcenx/wine && brew install --cask gcenx/wine/wine-stable && xattr -dr com.apple.quarantine \"/Applications/Wine Stable.app\"",
            installed: false
        )
    ]

    private var timer: Timer?

    private var scriptPath: String {
        if let res = Bundle.main.resourcePath {
            let bundled = (res as NSString).appendingPathComponent("setup_v34.sh")
            if FileManager.default.fileExists(atPath: bundled) {
                return bundled
            }
        }
        let cwd = FileManager.default.currentDirectoryPath
        return (cwd as NSString).appendingPathComponent("loader/setup_v34.sh")
    }

    var launchDir: URL {
        Bundle.main.bundleURL.deletingLastPathComponent()
    }

    var dropDir: URL {
        let appSupport = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let dir = appSupport.appendingPathComponent("IslamwareV34/drop", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    init() {
        refreshState()
        timer = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: true) { [weak self] _ in
            self?.refreshState()
        }
    }

    deinit {
        timer?.invalidate()
    }

    func refreshState() {
        if state == .installing || state == .launching { return }
        DispatchQueue.global(qos: .utility).async { [weak self] in
            guard let self = self else { return }
            let out = self.runScript(["status"]).output.trimmingCharacters(in: .whitespacesAndNewlines)
            let next: LoaderState
            switch out {
            case "installed":        next = .installed
            case "ready_to_install": next = .readyToInstall
            default:                 next = .needExe
            }

            let reqOut = self.runScript(["check-reqs"]).output
            var reqMap: [String: Bool] = [:]
            for line in reqOut.split(separator: "\n") {
                let parts = line.split(separator: "=", maxSplits: 1)
                if parts.count == 2 {
                    reqMap[String(parts[0]).trimmingCharacters(in: .whitespaces)] =
                        (String(parts[1]).trimmingCharacters(in: .whitespaces) == "1")
                }
            }

            DispatchQueue.main.async {
                if !reqMap.isEmpty {
                    var updated = self.requirements
                    for i in updated.indices {
                        if let val = reqMap[updated[i].id] {
                            updated[i].installed = val
                        }
                    }
                    if updated != self.requirements {
                        withAnimation(.easeInOut(duration: 0.18)) {
                            self.requirements = updated
                        }
                    }
                }
                if self.state != .installing && self.state != .launching && self.state != next {
                    withAnimation(.easeInOut(duration: 0.18)) {
                        self.state = next
                    }
                }
            }
        }
    }

    func openDropFolder() {
        NSWorkspace.shared.open(launchDir)
    }

    func acceptDroppedFile(url: URL) {
        guard url.pathExtension.lowercased() == "exe" else {
            statusNote = "Only .exe files are supported"
            return
        }
        let targetDir = FileManager.default.isWritableFile(atPath: launchDir.path) ? launchDir : dropDir
        let dest = targetDir.appendingPathComponent(url.lastPathComponent)
        do {
            if dest.standardizedFileURL != url.standardizedFileURL {
                if FileManager.default.fileExists(atPath: dest.path) {
                    try FileManager.default.removeItem(at: dest)
                }
                try FileManager.default.copyItem(at: url, to: dest)
            }
            statusNote = ""
            refreshState()
        } catch {
            statusNote = "Copy failed: \(error.localizedDescription)"
        }
    }

    func install() {
        statusNote = ""
        withAnimation(.easeInOut(duration: 0.15)) { state = .installing }
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            let res = self.runScript(["install"])
            DispatchQueue.main.async {
                if res.code == 0 {
                    self.statusNote = ""
                    withAnimation(.easeInOut(duration: 0.18)) { self.state = .installed }
                } else {
                    let trimmed = res.output.trimmingCharacters(in: .whitespacesAndNewlines)
                    self.statusNote = trimmed.isEmpty ? "Install failed (exit \(res.code))" : trimmed
                    withAnimation(.easeInOut(duration: 0.18)) { self.state = .readyToInstall }
                }
            }
        }
    }

    func launch() {
        statusNote = ""
        withAnimation(.easeInOut(duration: 0.15)) { state = .launching }
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            let res = self.runScript(["launch"])
            DispatchQueue.main.async {
                if res.code != 0 {
                    let trimmed = res.output.trimmingCharacters(in: .whitespacesAndNewlines)
                    self.statusNote = trimmed.isEmpty ? "Launch failed (exit \(res.code))" : trimmed
                } else {
                    self.statusNote = ""
                }
                withAnimation(.easeInOut(duration: 0.18)) { self.state = .installed }
            }
        }
    }

    func uninstall() {
        statusNote = ""
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            guard let self = self else { return }
            _ = self.runScript(["uninstall"])
            DispatchQueue.main.async {
                self.refreshState()
            }
        }
    }

    private func runScript(_ args: [String]) -> (code: Int32, output: String) {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/bin/bash")
        proc.arguments = [scriptPath] + args
        var env = ProcessInfo.processInfo.environment
        env["ISLAMWARE_LAUNCH_DIR"] = launchDir.path
        proc.environment = env
        let pipe = Pipe()
        proc.standardOutput = pipe
        proc.standardError = pipe
        do {
            try proc.run()
            proc.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            return (proc.terminationStatus, String(data: data, encoding: .utf8) ?? "")
        } catch {
            return (-1, error.localizedDescription)
        }
    }
}

struct RequirementFieldView: View {
    let item: RequirementItem
    let fieldBg: Color
    @State private var copied: Bool = false

    var body: some View {
        VStack(alignment: .leading, spacing: item.installed ? 0 : 8) {
            HStack(spacing: 8) {
                Image(systemName: item.installed ? "checkmark.circle.fill" : "terminal")
                    .font(.system(size: 12, weight: .medium))
                    .foregroundColor(item.installed ? Color.gray.opacity(0.65) : Color.white.opacity(0.85))

                Text(item.title)
                    .font(.system(size: 13, weight: .medium))
                    .foregroundColor(item.installed ? Color.gray.opacity(0.7) : Color.white.opacity(0.92))

                Spacer()

                if item.installed {
                    Text("Installed")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundColor(Color.gray.opacity(0.65))
                } else {
                    Button(action: {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(item.command, forType: .string)
                        copied = true
                        DispatchQueue.main.asyncAfter(deadline: .now() + 1.4) {
                            copied = false
                        }
                    }) {
                        Text(copied ? "Copied" : "Copy")
                            .font(.system(size: 11, weight: .medium))
                            .foregroundColor(copied ? Color.green.opacity(0.9) : Color.white.opacity(0.8))
                            .padding(.horizontal, 8)
                            .padding(.vertical, 3)
                            .background(
                                RoundedRectangle(cornerRadius: 6, style: .continuous)
                                    .fill(Color.white.opacity(0.08))
                            )
                    }
                    .buttonStyle(.plain)
                }
            }

            if !item.installed {
                Text(item.command)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(Color.white.opacity(0.78))
                    .lineLimit(nil)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 7)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(
                        RoundedRectangle(cornerRadius: 8, style: .continuous)
                            .fill(Color.black.opacity(0.32))
                    )
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, item.installed ? 9 : 10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(item.installed ? Color(red: 0.14, green: 0.14, blue: 0.14).opacity(0.55) : fieldBg)
                .overlay(
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .stroke(Color.white.opacity(item.installed ? 0.04 : 0.08), lineWidth: 1)
                )
        )
        .opacity(item.installed ? 0.55 : 1.0)
        .allowsHitTesting(!item.installed)
    }
}

struct LoaderView: View {
    @StateObject private var vm = LoaderViewModel()

    // Button background now uses the previous card background color; outer tab background is transparent.
    private let btnBg = Color(red: 0.115, green: 0.115, blue: 0.115)
    private let btnRed = Color(red: 0.24, green: 0.125, blue: 0.13)

    var body: some View {
        VStack(spacing: 12) {
            if vm.state == .needExe || vm.state == .readyToInstall {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Requirements (run in Terminal)")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundColor(.white.opacity(0.65))
                        .padding(.horizontal, 4)

                    ForEach(vm.requirements) { req in
                        RequirementFieldView(item: req, fieldBg: btnBg)
                    }
                }
            }

            switch vm.state {
            case .needExe:
                Button(action: { vm.openDropFolder() }) {
                    VStack(spacing: 6) {
                        Text("Place an .exe\nin the folder.")
                            .font(.system(size: 17, weight: .regular))
                            .foregroundColor(.white.opacity(0.92))
                            .multilineTextAlignment(.center)
                            .lineSpacing(3)
                    }
                    .frame(maxWidth: .infinity)
                    .frame(height: 96)
                    .background(
                        RoundedRectangle(cornerRadius: 18, style: .continuous)
                            .fill(btnBg)
                            .overlay(
                                RoundedRectangle(cornerRadius: 18, style: .continuous)
                                    .stroke(Color.white.opacity(vm.isHoveringDrop ? 0.30 : 0.08), lineWidth: 1)
                            )
                    )
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)

            case .readyToInstall:
                Button(action: { vm.install() }) {
                    Text("Install")
                        .font(.system(size: 24, weight: .medium))
                        .foregroundColor(.white)
                        .frame(maxWidth: .infinity)
                        .frame(height: 92)
                        .background(
                            RoundedRectangle(cornerRadius: 18, style: .continuous)
                                .fill(btnBg)
                                .overlay(
                                    RoundedRectangle(cornerRadius: 18, style: .continuous)
                                        .stroke(Color.white.opacity(0.08), lineWidth: 1)
                                )
                        )
                }
                .buttonStyle(.plain)

            case .installing:
                VStack(spacing: 12) {
                    ProgressView()
                        .controlSize(.small)
                        .colorScheme(.dark)
                    Text("Installing...")
                        .font(.system(size: 16, weight: .medium))
                        .foregroundColor(.white.opacity(0.85))
                }
                .frame(maxWidth: .infinity)
                .frame(height: 112)
                .background(
                    RoundedRectangle(cornerRadius: 18, style: .continuous)
                        .fill(btnBg)
                        .overlay(
                            RoundedRectangle(cornerRadius: 18, style: .continuous)
                                .stroke(Color.white.opacity(0.08), lineWidth: 1)
                        )
                )

            case .installed, .launching:
                VStack(spacing: 12) {
                    Button(action: { vm.uninstall() }) {
                        Text("Uninstall")
                            .font(.system(size: 20, weight: .medium))
                            .foregroundColor(.white)
                            .frame(maxWidth: .infinity)
                            .frame(height: 54)
                            .background(
                                RoundedRectangle(cornerRadius: 16, style: .continuous)
                                    .fill(btnRed)
                                    .overlay(
                                        RoundedRectangle(cornerRadius: 16, style: .continuous)
                                            .stroke(Color.white.opacity(0.08), lineWidth: 1)
                                    )
                            )
                    }
                    .buttonStyle(.plain)
                    .disabled(vm.state == .launching)

                    Button(action: { vm.launch() }) {
                        Group {
                            if vm.state == .launching {
                                HStack(spacing: 10) {
                                    ProgressView()
                                        .controlSize(.small)
                                        .colorScheme(.dark)
                                    Text("Launching...")
                                        .font(.system(size: 20, weight: .medium))
                                        .foregroundColor(.white.opacity(0.85))
                                }
                            } else {
                                Text("Launch")
                                    .font(.system(size: 26, weight: .medium))
                                    .foregroundColor(.white)
                            }
                        }
                        .frame(maxWidth: .infinity)
                        .frame(height: 112)
                        .background(
                            RoundedRectangle(cornerRadius: 18, style: .continuous)
                                .fill(btnBg)
                                .overlay(
                                    RoundedRectangle(cornerRadius: 18, style: .continuous)
                                        .stroke(Color.white.opacity(0.08), lineWidth: 1)
                                )
                        )
                    }
                    .buttonStyle(.plain)
                    .disabled(vm.state == .launching)
                }
            }

            if !vm.statusNote.isEmpty {
                ScrollView {
                    Text(vm.statusNote)
                        .font(.system(size: 11, design: .monospaced))
                        .foregroundColor(Color(red: 1.0, green: 0.62, blue: 0.62))
                        .lineLimit(nil)
                        .fixedSize(horizontal: false, vertical: true)
                        .multilineTextAlignment(.leading)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 110)
                .padding(10)
                .background(
                    RoundedRectangle(cornerRadius: 12, style: .continuous)
                        .fill(btnBg)
                        .overlay(
                            RoundedRectangle(cornerRadius: 12, style: .continuous)
                                .stroke(Color.red.opacity(0.35), lineWidth: 1)
                        )
                )
            }
        }
        .padding(.top, 28)
        .padding(.horizontal, 16)
        .padding(.bottom, 16)
        .frame(width: 340)
        .fixedSize(horizontal: false, vertical: true)
        .background(Color(red: 0.085, green: 0.085, blue: 0.085))
        .onDrop(of: [.fileURL], isTargeted: $vm.isHoveringDrop) { providers in
            guard let item = providers.first else { return false }
            item.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { data, _ in
                if let data = data as? Data, let url = URL(dataRepresentation: data, relativeTo: nil) {
                    DispatchQueue.main.async {
                        vm.acceptDroppedFile(url: url)
                    }
                }
            }
            return true
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow!

    func applicationDidFinishLaunching(_ notification: Notification) {
        let hosting = NSHostingView(rootView: LoaderView())

        let win = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 340, height: 340),
            styleMask: [.titled, .closable, .miniaturizable, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )
        win.titleVisibility = .hidden
        win.titlebarAppearsTransparent = true
        win.isMovableByWindowBackground = true
        win.backgroundColor = NSColor(red: 0.085, green: 0.085, blue: 0.085, alpha: 1.0)
        win.appearance = NSAppearance(named: .darkAqua)
        win.contentView = hosting
        win.center()
        win.makeKeyAndOrderFront(nil)
        self.window = win
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
