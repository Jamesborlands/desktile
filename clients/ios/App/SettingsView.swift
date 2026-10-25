import SwiftUI
import WidgetKit

struct SettingsView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var url = TodoAPI.baseURL
    @State private var token = Keychain.token() ?? ""
    @State private var status: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Server URL", text: $url)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    SecureField("API token", text: $token)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                } footer: {
                    Text("The token is SKYTILE_API_TOKEN from the .env file in the desktile repo. It's stored in the keychain and shared with the widget.")
                }
                Section {
                    Button("Save and test") { Task { await saveAndTest() } }
                    if let status { Text(status).foregroundStyle(.secondary) }
                }
            }
            .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { save(); dismiss() } }
            }
        }
    }

    private func save() {
        TodoAPI.baseURL = url.trimmingCharacters(in: .whitespacesAndNewlines)
        if !Keychain.setToken(token.trimmingCharacters(in: .whitespacesAndNewlines)) {
            status = "Couldn't save the token to the keychain."
        }
        WidgetCenter.shared.reloadAllTimelines()
    }

    private func saveAndTest() async {
        save()
        status = "Testing..."
        do {
            let todos = try await TodoAPI.list()
            status = "Connected. \(todos.open.count) open to-do\(todos.open.count == 1 ? "" : "s")."
        } catch {
            status = error.localizedDescription
        }
    }
}
