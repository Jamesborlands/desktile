import Foundation

enum APIError: LocalizedError {
    case notConfigured
    case http(Int)

    var errorDescription: String? {
        switch self {
        case .notConfigured: return "Add your API token in Settings."
        case .http(401): return "The API token was rejected. Check it in Settings."
        case .http(let code): return "SkyTile server error (HTTP \(code))."
        }
    }
}

// Client for the SkyTile Worker's /api/v1/todos (Bearer token). Every call that returns
// the list also refreshes the shared cache.
struct TodoAPI {
    static var baseURL: String {
        get { Shared.defaults.string(forKey: "url") ?? Shared.defaultURL }
        set { Shared.defaults.set(newValue, forKey: "url") }
    }

    private struct ListResponse: Decodable { let todos: [Todo] }
    private struct ItemResponse: Decodable { let todo: Todo }

    private static func request(_ method: String, _ path: String, body: [String: String]? = nil) async throws -> Data {
        guard let token = Keychain.token(), !token.isEmpty,
              let url = URL(string: baseURL.trimmingCharacters(in: CharacterSet(charactersIn: "/ ")) + "/api/v1/todos" + path)
        else { throw APIError.notConfigured }
        var req = URLRequest(url: url, timeoutInterval: 10)
        req.httpMethod = method
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
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
        let todos = try JSONDecoder().decode(ListResponse.self, from: try await request("GET", "")).todos
        TodoCache.save(todos)
        return todos
    }

    @discardableResult
    static func add(_ text: String, source: String) async throws -> Todo {
        try JSONDecoder().decode(ItemResponse.self, from: try await request("POST", "", body: ["text": text, "source": source])).todo
    }

    @discardableResult
    static func setDone(_ id: String, _ done: Bool) async throws -> Todo {
        try JSONDecoder().decode(ItemResponse.self, from: try await request("POST", "/\(id)/\(done ? "complete" : "uncomplete")")).todo
    }

    static func remove(_ id: String) async throws {
        _ = try await request("DELETE", "/\(id)")
    }
}
