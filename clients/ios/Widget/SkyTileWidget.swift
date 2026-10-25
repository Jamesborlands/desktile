import AppIntents
import SwiftUI
import WidgetKit

struct TodoEntry: TimelineEntry {
    let date: Date
    let todos: [Todo]
    var error: String? = nil
}

// Fetches the list when iOS asks (roughly every 15 min; iOS sets the real budget), and
// right after a checkbox tap. Falls back to the cached list when offline.
struct Provider: TimelineProvider {
    func placeholder(in context: Context) -> TodoEntry {
        TodoEntry(date: .now, todos: [
            Todo(id: "1", text: "Pay estimated taxes", done: false),
            Todo(id: "2", text: "Buy the Amazon order", done: false),
            Todo(id: "3", text: "Send pre-read", done: false),
        ])
    }

    func getSnapshot(in context: Context, completion: @escaping (TodoEntry) -> Void) {
        let cached = TodoCache.load()
        completion(context.isPreview && cached.isEmpty ? placeholder(in: context) : TodoEntry(date: .now, todos: cached))
    }

    func getTimeline(in context: Context, completion: @escaping (Timeline<TodoEntry>) -> Void) {
        Task {
            var entry: TodoEntry
            do {
                entry = TodoEntry(date: .now, todos: try await TodoAPI.list())
            } catch {
                entry = TodoEntry(date: .now, todos: TodoCache.load(), error: error.localizedDescription)
            }
            completion(Timeline(entries: [entry], policy: .after(.now.addingTimeInterval(15 * 60))))
        }
    }
}

// ---------------------------------------------------------------- home screen
struct TodoRow: View {
    let todo: Todo
    var font: Font = .subheadline

    var body: some View {
        Button(intent: CompleteTodoIntent(todoID: todo.id, done: !todo.done)) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Image(systemName: todo.done ? "checkmark.circle.fill" : "circle")
                    .foregroundStyle(todo.done ? .secondary : .primary)
                    .imageScale(.medium)
                Text(todo.text)
                    .font(font)
                    .lineLimit(1)
                    .strikethrough(todo.done)
                    .foregroundStyle(todo.done ? .secondary : .primary)
                Spacer(minLength: 0)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

struct HomeView: View {
    let entry: TodoEntry
    let maxRows: Int
    let showAdd: Bool

    var body: some View {
        let open = entry.todos.open
        VStack(alignment: .leading, spacing: maxRows > 4 ? 9 : 7) {
            HStack {
                Text("To-do").font(.headline)
                Spacer()
                if showAdd {
                    Link(destination: URL(string: "skytile://add")!) {
                        Image(systemName: "plus.circle.fill").font(.title3)
                    }
                } else {
                    Text("\(open.count)").font(.headline).foregroundStyle(.secondary)
                }
            }
            if open.isEmpty {
                Spacer()
                Text(entry.error ?? "All done").font(.subheadline).foregroundStyle(.secondary)
                Spacer()
            } else {
                ForEach(open.prefix(maxRows)) { TodoRow(todo: $0) }
                Spacer(minLength: 0)
                if open.count > maxRows {
                    Text("+\(open.count - maxRows) more").font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }
}

// ---------------------------------------------------------------- lock screen
struct CircularView: View {
    let entry: TodoEntry
    var body: some View {
        ZStack {
            AccessoryWidgetBackground()
            VStack(spacing: 0) {
                Image(systemName: "checklist").font(.caption)
                Text("\(entry.todos.open.count)").font(.title3.bold())
            }
        }
    }
}

struct RectangularView: View {
    let entry: TodoEntry
    var body: some View {
        let open = entry.todos.open
        VStack(alignment: .leading, spacing: 1) {
            Label("\(open.count) to-do\(open.count == 1 ? "" : "s")", systemImage: "checklist").font(.headline)
            ForEach(open.prefix(2)) { t in
                Text(t.text).font(.caption).lineLimit(1)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct EntryView: View {
    @Environment(\.widgetFamily) var family
    let entry: TodoEntry

    var body: some View {
        switch family {
        case .systemSmall: HomeView(entry: entry, maxRows: 3, showAdd: false)
        case .systemMedium: HomeView(entry: entry, maxRows: 4, showAdd: true)
        case .systemLarge: HomeView(entry: entry, maxRows: 10, showAdd: true)
        case .accessoryCircular: CircularView(entry: entry)
        case .accessoryRectangular: RectangularView(entry: entry)
        case .accessoryInline:
            Label("\(entry.todos.open.count) to-dos" + (entry.todos.open.first.map { ": \($0.text)" } ?? ""), systemImage: "checklist")
        default: HomeView(entry: entry, maxRows: 4, showAdd: true)
        }
    }
}

struct SkyTileWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: "SkyTileTodos", provider: Provider()) { entry in
            EntryView(entry: entry)
                .containerBackground(.fill.tertiary, for: .widget)
                .widgetURL(URL(string: "skytile://list"))
        }
        .configurationDisplayName("SkyTile To-do")
        .description("Your desk tile's to-do list. Tap a circle to check it off.")
        .supportedFamilies([.systemSmall, .systemMedium, .systemLarge, .accessoryCircular, .accessoryRectangular, .accessoryInline])
    }
}

@main
struct SkyTileWidgets: WidgetBundle {
    var body: some Widget {
        SkyTileWidget()
    }
}
