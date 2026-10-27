import AppKit
import ServiceManagement
import SwiftUI

// The dropdown: an always-ready add field, the open list (click the circle to check off,
// double-click the text to edit in place, hover for edit/delete), then Completed.
struct TodoPanel: View {
    @EnvironmentObject private var store: TodoStore
    @State private var draft = ""
    @State private var editing: String?
    @State private var showDone = true
    @State private var listHeight: CGFloat = 0
    @FocusState private var addFocused: Bool

    var body: some View {
        GlassEffectContainer(spacing: 8) {
            VStack(spacing: 8) {
                addField
                if let error = store.error { errorBanner(error) }
                list
                footer
            }
        }
        .padding(10)
        .frame(width: 310)
        .onAppear { focusAdd() }
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.didBecomeKeyNotification)) { _ in
            store.refresh()
            if editing == nil { focusAdd() }
        }
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            Text("To-do").font(.subheadline.weight(.semibold))
            Spacer()
            Text(store.open.isEmpty ? "All done" : "\(store.open.count) open")
                .font(.subheadline)
                .foregroundStyle(.secondary)
                .contentTransition(.numericText())
        }
        .padding(.horizontal, 8)
        .padding(.top, 4)
        .padding(.bottom, 1)
    }

    private var addField: some View {
        HStack(spacing: 9) {
            Button(action: submit) {
                Image(systemName: "plus")
                    .font(.system(size: 13, weight: .bold))
                    .foregroundStyle(.tint)
                    .frame(width: 28, height: 28)
                    .background(.white, in: .circle)
                    .contentShape(.circle)
            }
            .buttonStyle(.plain)
            .help("Add to-do")
            TextField("New to-do", text: $draft)
                .textFieldStyle(.plain)
                .font(.body.weight(.medium))
                .focused($addFocused)
                .onSubmit(submit)
                .onChange(of: draft) { _, v in if v.count > TodoAPI.maxText { draft = String(v.prefix(TodoAPI.maxText)) } }
            if draft.count > TodoAPI.maxText - 20 {
                Text("\(TodoAPI.maxText - draft.count)").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
            if !draft.isEmpty {
                Image(systemName: "return").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            }
        }
        .padding(.leading, 6)
        .padding(.trailing, 14)
        .padding(.vertical, 6)
        .glassEffect(.regular.interactive(), in: .capsule)
    }

    private func errorBanner(_ message: String) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
            Text(message).font(.callout).fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            Button("Retry", action: store.refresh).buttonStyle(.glass).controlSize(.small)
        }
        .padding(14)
        .glassEffect(.regular.tint(.orange.opacity(0.25)), in: .rect(cornerRadius: 24))
    }

    private var list: some View {
        ScrollView {
            VStack(spacing: 8) {
                module {
                    header
                    if store.open.isEmpty {
                        emptyState
                    }
                    ForEach(store.open) { todo in
                        TodoRow(todo: todo, editing: $editing, onEditEnd: focusAdd)
                            .transition(.asymmetric(insertion: .push(from: .top).combined(with: .opacity), removal: .opacity))
                    }
                }
                if !store.done.isEmpty {
                    module {
                        completedHeader
                        if showDone {
                            ForEach(store.done) { todo in
                                TodoRow(todo: todo, editing: $editing, onEditEnd: focusAdd)
                                    .transition(.opacity)
                            }
                        }
                    }
                }
            }
            .animation(.snappy(duration: 0.3), value: store.todos)
            .onGeometryChange(for: CGFloat.self) { $0.size.height } action: { listHeight = $0 }
        }
        .scrollBounceBehavior(.basedOnSize)
        .frame(height: min(max(listHeight, 1), 400))
    }

    // A Control Center style module: one glass slab with a light rim.
    private func module<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 2, content: content)
            .padding(5)
            .frame(maxWidth: .infinity, alignment: .leading)
            .glassEffect(.regular, in: .rect(cornerRadius: 18))
    }

    private var emptyState: some View {
        VStack(spacing: 6) {
            Image(systemName: "checkmark.circle").font(.system(size: 28, weight: .light)).foregroundStyle(.tertiary)
            Text("Nothing to do").font(.callout).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 18)
    }

    private var completedHeader: some View {
        HStack {
            Button {
                withAnimation(.snappy) { showDone.toggle() }
            } label: {
                HStack(spacing: 4) {
                    Text("Completed").font(.subheadline.weight(.semibold))
                    Text("\(store.done.count)").font(.subheadline).foregroundStyle(.secondary)
                    Image(systemName: "chevron.right")
                        .font(.caption2.weight(.bold))
                        .foregroundStyle(.secondary)
                        .rotationEffect(.degrees(showDone ? 90 : 0))
                }
                .contentShape(.rect)
            }
            .buttonStyle(.plain)
            Spacer()
            Button("Clear", action: store.clearCompleted)
                .buttonStyle(.plain)
                .font(.subheadline)
                .foregroundStyle(.tint)
                .help("Remove completed items now (they clear on their own after 24 hours)")
        }
        .padding(.horizontal, 8)
        .padding(.top, 4)
        .padding(.bottom, 1)
    }

    private var footer: some View {
        HStack {
            TimelineView(.periodic(from: .now, by: 15)) { context in
                Text(updatedText(now: context.date)).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            Menu {
                Button("Refresh", systemImage: "arrow.clockwise", action: store.refresh).keyboardShortcut("r")
                Toggle("Open at Login", isOn: launchAtLogin)
                Button("Edit Config File…", systemImage: "doc.text") {
                    NSWorkspace.shared.open(URL(fileURLWithPath: TodoAPI.configPath))
                }
                Divider()
                Button("Quit SkyTile", systemImage: "power") { NSApp.terminate(nil) }.keyboardShortcut("q")
            } label: {
                Image(systemName: "ellipsis")
            }
            .menuIndicator(.hidden)
            .buttonStyle(.glass)
            .buttonBorderShape(.circle)
            .fixedSize()
        }
        .padding(.horizontal, 6)
    }

    private var launchAtLogin: Binding<Bool> {
        Binding(
            get: { SMAppService.mainApp.status == .enabled },
            set: { on in try? on ? SMAppService.mainApp.register() : SMAppService.mainApp.unregister() }
        )
    }

    private func updatedText(now: Date) -> String {
        guard let updated = store.updated else { return "⌥⌘T opens this from anywhere" }
        if now.timeIntervalSince(updated) < 60 { return "Updated just now" }
        return "Updated " + RelativeDateTimeFormatter().localizedString(for: updated, relativeTo: now)
    }

    private func submit() {
        store.add(draft)
        draft = ""
        focusAdd()
    }

    private func focusAdd() {
        DispatchQueue.main.async { addFocused = true }
    }
}

private struct TodoRow: View {
    @EnvironmentObject private var store: TodoStore
    let todo: Todo
    @Binding var editing: String?
    var onEditEnd: () -> Void

    @State private var hovering = false
    @State private var checking = false
    @State private var text = ""
    @FocusState private var fieldFocused: Bool

    private var isEditing: Bool { editing == todo.id }
    private var checked: Bool { todo.done || checking }

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            checkbox
            if isEditing {
                TextField("To-do", text: $text, axis: .vertical)
                    .textFieldStyle(.plain)
                    .focused($fieldFocused)
                    .onSubmit(save)
                    .onExitCommand(perform: cancel)
                    .onChange(of: text) { _, v in if v.count > TodoAPI.maxText { text = String(v.prefix(TodoAPI.maxText)) } }
                    .onChange(of: fieldFocused) { _, f in if !f && isEditing { save() } }
            } else {
                Text(todo.text)
                    .strikethrough(todo.done, color: .secondary)
                    .foregroundStyle(todo.done ? .secondary : .primary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fixedSize(horizontal: false, vertical: true)
                    .contentShape(.rect)
                    .onTapGesture(count: 2, perform: beginEdit)
            }
            if hovering && !isEditing { actions }
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(isEditing ? Color.accentColor.opacity(0.15) : hovering ? Color.primary.opacity(0.08) : .clear)
        }
        .onHover { hovering = $0 }
        .contextMenu {
            Button(todo.done ? "Mark as Not Done" : "Mark as Done", systemImage: todo.done ? "circle" : "checkmark.circle") {
                store.setDone(todo, !todo.done)
            }
            Button("Edit", systemImage: "pencil", action: beginEdit)
            Button("Copy", systemImage: "doc.on.doc") {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(todo.text, forType: .string)
            }
            Divider()
            Button("Delete", systemImage: "trash", role: .destructive) { store.remove(todo) }
        }
    }

    private var checkbox: some View {
        Button(action: toggle) {
            Image(systemName: checked ? "checkmark.circle.fill" : "circle")
                .font(.system(size: 15))
                .foregroundStyle(checked ? (todo.done ? AnyShapeStyle(.secondary) : AnyShapeStyle(.tint)) : AnyShapeStyle(.secondary))
                .contentTransition(.symbolEffect(.replace))
                .frame(width: 18, height: 18)
                .contentShape(.circle)
        }
        .buttonStyle(.plain)
        .help(todo.done ? "Mark as not done" : "Mark as done")
    }

    private var actions: some View {
        HStack(spacing: 2) {
            if !todo.done {
                iconButton("pencil", help: "Edit", action: beginEdit)
            }
            iconButton("trash", help: "Delete") { store.remove(todo) }
        }
        .transition(.opacity)
    }

    private func iconButton(_ symbol: String, help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(.secondary)
                .frame(width: 24, height: 22)
                .contentShape(.rect)
        }
        .buttonStyle(.plain)
        .help(help)
    }

    private func toggle() {
        if todo.done {
            store.setDone(todo, false)
            return
        }
        // Show the check for a beat before the row slides to Completed, like Reminders.
        checking = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35) {
            store.setDone(todo, true)
            checking = false
        }
    }

    private func beginEdit() {
        text = todo.text
        editing = todo.id
        DispatchQueue.main.async { fieldFocused = true }
    }

    private func save() {
        guard isEditing else { return }
        store.edit(todo, to: text)
        editing = nil
        onEditEnd()
    }

    private func cancel() {
        editing = nil
        onEditEnd()
    }
}
