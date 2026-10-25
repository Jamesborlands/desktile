import AppIntents
import WidgetKit

// Checkbox in the widget: runs in the background without opening the app (iOS 17+).
// The cache is updated first so the widget redraws with the item checked even before
// the server answers; it's rolled back if the request fails.
struct CompleteTodoIntent: AppIntent {
    static var title: LocalizedStringResource = "Complete To-do"
    static var isDiscoverable = false

    @Parameter(title: "To-do ID") var todoID: String
    @Parameter(title: "Done", default: true) var done: Bool

    init() {}
    init(todoID: String, done: Bool = true) {
        self.todoID = todoID
        self.done = done
    }

    func perform() async throws -> some IntentResult {
        TodoCache.setDone(todoID, done)
        do {
            try await TodoAPI.setDone(todoID, done)
        } catch {
            TodoCache.setDone(todoID, !done)
            throw error
        }
        _ = try? await TodoAPI.list()
        WidgetCenter.shared.reloadAllTimelines()
        return .result()
    }
}

// "Add a to-do in SkyTile" for Siri, Shortcuts, Spotlight and the Action button.
struct AddTodoIntent: AppIntent {
    static var title: LocalizedStringResource = "Add To-do"
    static var description = IntentDescription("Add an item to the SkyTile to-do list.")

    @Parameter(title: "To-do", requestValueDialog: "What should I add?")
    var text: String

    static var parameterSummary: some ParameterSummary {
        Summary("Add \(\.$text) to SkyTile")
    }

    func perform() async throws -> some IntentResult & ProvidesDialog {
        let item = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !item.isEmpty else { return .result(dialog: "Nothing to add.") }
        let todo = try await TodoAPI.add(String(item.prefix(120)), source: "siri")
        _ = try? await TodoAPI.list()
        WidgetCenter.shared.reloadAllTimelines()
        return .result(dialog: "Added \(todo.text).")
    }
}
