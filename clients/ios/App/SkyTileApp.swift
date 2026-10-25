import AppIntents
import SwiftUI
import WidgetKit

@main
struct SkyTileApp: App {
    @StateObject private var model = TodoModel()

    init() {
        #if DEBUG
        // Simulator testing: launch with `-SKYTILE_TOKEN <token>` to skip typing it in Settings.
        if let t = UserDefaults.standard.string(forKey: "SKYTILE_TOKEN") { Keychain.setToken(t) }
        #endif
    }

    var body: some Scene {
        WindowGroup {
            ContentView().environmentObject(model)
        }
    }
}

// Siri / Shortcuts phrases. Siri asks "What should I add?" after the phrase.
struct SkyTileShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(
            intent: AddTodoIntent(),
            phrases: [
                "Add a to-do in \(.applicationName)",
                "Add to my \(.applicationName) list",
                "New \(.applicationName) to-do",
            ],
            shortTitle: "Add To-do",
            systemImageName: "plus.circle"
        )
    }
}

// The list as the app sees it. Changes show locally at once, then go to the server;
// a failure reloads from the server and shows the error.
@MainActor
final class TodoModel: ObservableObject {
    @Published var todos: [Todo] = TodoCache.load()
    @Published var error: String?
    @Published var loading = false

    var configured: Bool { !(Keychain.token() ?? "").isEmpty }

    func refresh() async {
        guard configured else { error = APIError.notConfigured.localizedDescription; return }
        loading = true
        defer { loading = false }
        do {
            todos = try await TodoAPI.list()
            error = nil
        } catch {
            self.error = error.localizedDescription
        }
    }

    func add(_ text: String) async {
        let item = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !item.isEmpty else { return }
        await run { try await TodoAPI.add(String(item.prefix(120)), source: "ios") }
    }

    func setDone(_ todo: Todo, _ done: Bool) async {
        if let i = todos.firstIndex(of: todo) {
            todos[i].done = done
            TodoCache.save(todos)
        }
        await run { try await TodoAPI.setDone(todo.id, done) }
    }

    func remove(_ todo: Todo) async {
        todos.removeAll { $0.id == todo.id }
        await run { try await TodoAPI.remove(todo.id) }
    }

    private func run(_ op: () async throws -> Void) async {
        do {
            try await op()
            error = nil
        } catch {
            self.error = error.localizedDescription
        }
        await refresh()
        WidgetCenter.shared.reloadAllTimelines()
    }
}
