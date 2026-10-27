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

enum APIError: LocalizedError {
    case notConfigured
    case http(Int)

    var errorDescription: String? {
        switch self {
        case .notConfigured: return "Add SKYTILE_URL and SKYTILE_API_TOKEN to \(TodoAPI.configPath)."
        case .http(401): return "The API token was rejected. Check \(TodoAPI.configPath)."
        case .http(let code): return "SkyTile server error (HTTP \(code))."
        }
    }
}

// Client for /api/v1/todos. Reads its config from
// ~/.config/skytile/config (KEY=value lines, same format as the repo's .env).
enum TodoAPI {
    static let maxText = 120
    static var configPath: String {
        ProcessInfo.processInfo.environment["SKYTILE_CONFIG"] ?? NSString(string: "~/.config/skytile/config").expandingTildeInPath
    }

    private struct ListResponse: Decodable { let todos: [Todo] }
    private struct ItemResponse: Decodable { let todo: Todo }

    private static func config() -> (url: String, token: String)? {
        guard let text = try? String(contentsOfFile: configPath, encoding: .utf8) else { return nil }
        var cfg: [String: String] = [:]
        for line in text.split(whereSeparator: \.isNewline) {
            let line = line.trimmingCharacters(in: .whitespaces)
            guard !line.hasPrefix("#"), let eq = line.firstIndex(of: "=") else { continue }
            cfg[line[..<eq].trimmingCharacters(in: .whitespaces)] =
                line[line.index(after: eq)...].trimmingCharacters(in: CharacterSet(charactersIn: " \"'"))
        }
        guard let url = cfg["SKYTILE_URL"], let token = cfg["SKYTILE_API_TOKEN"], !url.isEmpty, !token.isEmpty else { return nil }
        return (url.trimmingCharacters(in: CharacterSet(charactersIn: "/")), token)
    }

    private static func request(_ method: String, _ path: String, body: [String: String]? = nil) async throws -> Data {
        guard let cfg = config(), let url = URL(string: cfg.url + "/api/v1/todos" + path) else { throw APIError.notConfigured }
        var req = URLRequest(url: url, timeoutInterval: 10)
        req.httpMethod = method
        req.setValue("Bearer \(cfg.token)", forHTTPHeaderField: "Authorization")
        req.setValue("SkyTile-Mac/1.0", forHTTPHeaderField: "User-Agent")
        if let body {
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
            req.httpBody = try JSONEncoder().encode(body)
        }
        let (data, resp) = try await URLSession.shared.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(code) else { throw APIError.http(code) }
        return data
    }

    static func list() async throws -> [Todo] {
        try JSONDecoder().decode(ListResponse.self, from: try await request("GET", "")).todos
    }

    static func add(_ text: String) async throws -> Todo {
        try JSONDecoder().decode(ItemResponse.self, from: try await request("POST", "", body: ["text": text, "source": "mac"])).todo
    }

    static func edit(_ id: String, _ text: String) async throws {
        _ = try await request("PATCH", "/\(id)", body: ["text": text])
    }

    static func setDone(_ id: String, _ done: Bool) async throws {
        _ = try await request("POST", "/\(id)/\(done ? "complete" : "uncomplete")")
    }

    static func remove(_ id: String) async throws {
        _ = try await request("DELETE", "/\(id)")
    }

    static func clearCompleted() async throws {
        _ = try await request("POST", "/clear-completed")
    }
}

// The list the menu bar shows. Changes are applied locally first so the UI never waits
// on the network; if a call fails the error is shown and the list is reloaded.
@MainActor
final class TodoStore: ObservableObject {
    @Published private(set) var todos: [Todo] = []
    @Published private(set) var error: String?
    @Published private(set) var updated: Date?
    private var timer: Timer?

    var open: [Todo] { todos.filter { !$0.done } }
    var done: [Todo] { todos.filter { $0.done } }

    func start() {
        guard timer == nil else { return }
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func refresh() {
        sync { self.todos = try await TodoAPI.list() }
    }

    func add(_ raw: String) {
        let text = String(raw.trimmingCharacters(in: .whitespacesAndNewlines).prefix(TodoAPI.maxText))
        guard !text.isEmpty else { return }
        let temp = Todo(id: "new-\(UUID().uuidString)", text: text, done: false)
        todos.insert(temp, at: todos.firstIndex(where: \.done) ?? todos.endIndex)
        sync {
            let saved = try await TodoAPI.add(text)
            if let i = self.todos.firstIndex(where: { $0.id == temp.id }) { self.todos[i] = saved }
        }
    }

    func edit(_ todo: Todo, to raw: String) {
        let text = String(raw.trimmingCharacters(in: .whitespacesAndNewlines).prefix(TodoAPI.maxText))
        guard !text.isEmpty, text != todo.text, let i = todos.firstIndex(where: { $0.id == todo.id }) else { return }
        todos[i].text = text
        sync { try await TodoAPI.edit(todo.id, text) }
    }

    func setDone(_ todo: Todo, _ done: Bool) {
        guard var t = todos.first(where: { $0.id == todo.id }) else { return }
        todos.removeAll { $0.id == todo.id }
        t.done = done
        t.doneAt = done ? Date().timeIntervalSince1970 * 1000 : nil
        // Open items keep their order; newly done ones go to the top of Completed.
        todos.insert(t, at: done ? (todos.firstIndex(where: \.done) ?? todos.endIndex) : openIndex(for: t))
        sync { try await TodoAPI.setDone(todo.id, done) }
    }

    func remove(_ todo: Todo) {
        todos.removeAll { $0.id == todo.id }
        sync { try await TodoAPI.remove(todo.id) }
    }

    func clearCompleted() {
        todos.removeAll(where: \.done)
        sync { try await TodoAPI.clearCompleted() }
    }

    private func openIndex(for t: Todo) -> Int {
        let open = todos.prefix { !$0.done }
        return open.firstIndex { ($0.position ?? .max) > (t.position ?? .max) } ?? open.endIndex
    }

    private func sync(_ op: @escaping @MainActor () async throws -> Void) {
        Task {
            do {
                try await op()
                error = nil
                updated = Date()
            } catch {
                self.error = error.localizedDescription
                if let fresh = try? await TodoAPI.list() { todos = fresh }
            }
        }
    }
}
