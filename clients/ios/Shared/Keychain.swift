import Foundation
import Security

// The API token lives in the keychain under the App Group's access group, so the app
// and the widget both read the same item. AfterFirstUnlock lets the widget refresh
// while the phone is locked.
enum Keychain {
    private static let service = "SkyTile"
    private static let account = "api-token"

    private static var query: [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecAttrAccessGroup as String: Shared.appGroup,
        ]
    }

    static func token() -> String? {
        var q = query
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: AnyObject?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let data = out as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    @discardableResult
    static func setToken(_ token: String) -> Bool {
        SecItemDelete(query as CFDictionary)
        guard !token.isEmpty else { return true }
        var q = query
        q[kSecValueData as String] = Data(token.utf8)
        q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        return SecItemAdd(q as CFDictionary, nil) == errSecSuccess
    }
}
