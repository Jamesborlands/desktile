import Foundation

// One item from the SkyTile Worker's /api/v1/todos.
struct Todo: Codable, Identifiable, Hashable {
    let id: String
    var text: String
    var done: Bool
    var position: Int?
    var source: String?
    var created: Double?
    var doneAt: Double?
}

enum Shared {
    // App Group shared by the app and the widget: holds the URL, the cached list and
    // (as a keychain access group) the API token. Set in project.yml (APP_GROUP).
    static let appGroup = Bundle.main.object(forInfoDictionaryKey: "SkyTileAppGroup") as? String ?? "group.com.seobubbles.skytile"
    static let defaults = UserDefaults(suiteName: appGroup) ?? .standard
    static let defaultURL = "https://skytile.seobubbles.workers.dev"
}

// Last list we saw, so the widget can draw something offline and the app opens instantly.
enum TodoCache {
    private static let key = "todos"

    static func load() -> [Todo] {
        guard let data = Shared.defaults.data(forKey: key) else { return [] }
        return (try? JSONDecoder().decode([Todo].self, from: data)) ?? []
    }

    static func save(_ todos: [Todo]) {
        if let data = try? JSONEncoder().encode(todos) { Shared.defaults.set(data, forKey: key) }
    }

    static func setDone(_ id: String, _ done: Bool) {
        var todos = load()
        if let i = todos.firstIndex(where: { $0.id == id }) {
            todos[i].done = done
            todos[i].doneAt = done ? Date().timeIntervalSince1970 * 1000 : nil
            save(todos)
        }
    }
}

extension Array where Element == Todo {
    var open: [Todo] { filter { !$0.done } }
    var completed: [Todo] { filter { $0.done } }
}
