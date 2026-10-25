import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: TodoModel
    @Environment(\.scenePhase) private var scenePhase
    @State private var draft = ""
    @State private var showSettings = false
    @FocusState private var addFocused: Bool

    // While the app is open, pick up changes from the tile, Mac or Claude.
    private let poll = Timer.publish(every: 10, on: .main, in: .common).autoconnect()

    var body: some View {
        NavigationStack {
            List {
                Section {
                    HStack {
                        TextField("Add a to-do", text: $draft)
                            .focused($addFocused)
                            .submitLabel(.done)
                            .onSubmit(add)
                        if !draft.isEmpty {
                            Button(action: add) { Image(systemName: "plus.circle.fill").font(.title2) }
                                .buttonStyle(.borderless)
                        }
                    }
                }
                if let error = model.error {
                    Section { Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.orange) }
                }
                Section {
                    if model.todos.open.isEmpty {
                        Text("All done").foregroundStyle(.secondary)
                    }
                    ForEach(model.todos.open) { row($0) }
                }
                if !model.todos.completed.isEmpty {
                    Section("Done") {
                        ForEach(model.todos.completed) { row($0) }
                    }
                }
            }
            .navigationTitle("To-do")
            .refreshable { await model.refresh() }
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showSettings = true } label: { Image(systemName: "gearshape") }
                }
            }
            .sheet(isPresented: $showSettings, onDismiss: { Task { await model.refresh() } }) {
                SettingsView()
            }
        }
        .task {
            if !model.configured { showSettings = true }
            await model.refresh()
        }
        .onChange(of: scenePhase) { _, phase in
            if phase == .active { Task { await model.refresh() } }
        }
        .onReceive(poll) { _ in
            if scenePhase == .active && !model.loading { Task { await model.refresh() } }
        }
        .onOpenURL { url in
            if url.host == "add" { addFocused = true }
        }
    }

    private func row(_ todo: Todo) -> some View {
        Button {
            Task { await model.setDone(todo, !todo.done) }
        } label: {
            HStack(spacing: 12) {
                Image(systemName: todo.done ? "checkmark.circle.fill" : "circle")
                    .font(.title3)
                    .foregroundStyle(todo.done ? Color.secondary : Color.accentColor)
                Text(todo.text)
                    .strikethrough(todo.done)
                    .foregroundStyle(todo.done ? .secondary : .primary)
                Spacer(minLength: 0)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .swipeActions(edge: .leading) {
            Button { Task { await model.setDone(todo, !todo.done) } } label: {
                Label(todo.done ? "Undo" : "Done", systemImage: todo.done ? "arrow.uturn.backward" : "checkmark")
            }
            .tint(.green)
        }
        .swipeActions(edge: .trailing) {
            Button(role: .destructive) { Task { await model.remove(todo) } } label: { Label("Delete", systemImage: "trash") }
        }
    }

    private func add() {
        let text = draft
        draft = ""
        addFocused = true  // keep the keyboard up for the next one
        Task { await model.add(text) }
    }
}
