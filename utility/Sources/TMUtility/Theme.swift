import AppKit
import SwiftUI

// Shared look: opaque surfaces (no sidebar/toolbar vibrancy) and System
// Settings-style colored icon tiles.

enum Theme {
    /// Page background (light gray / dark gray).
    static let window = Color(nsColor: .windowBackgroundColor)
    /// Cards, header bar, sidebar (white / near-black).
    static let surface = Color(nsColor: .controlBackgroundColor)
    static let border = Color(nsColor: .separatorColor)
    static let radius: CGFloat = 10
}

/// White SF Symbol on a colored rounded square.
struct IconTile: View {
    let symbol: String
    let color: Color
    var size: CGFloat = 22

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: size * 0.52, weight: .semibold))
            .foregroundColor(.white)
            .frame(width: size, height: size)
            .background(
                RoundedRectangle(cornerRadius: size * 0.26, style: .continuous)
                    .fill(LinearGradient(colors: [color.opacity(0.78), color], startPoint: .top, endPoint: .bottom))
            )
            .shadow(color: color.opacity(0.25), radius: 1, y: 0.5)
    }
}

extension View {
    /// Opaque rounded card surface.
    func cardSurface(padding: CGFloat = 14) -> some View {
        self.padding(padding)
            .background(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).fill(Theme.surface))
            .overlay(RoundedRectangle(cornerRadius: Theme.radius, style: .continuous).strokeBorder(Theme.border.opacity(0.6)))
            .shadow(color: .black.opacity(0.05), radius: 2, y: 1)
    }

    /// Opaque window: page background + solid toolbar.
    func opaqueWindow() -> some View {
        self.background(Theme.window)
            .toolbarBackground(Theme.surface, for: .windowToolbar)
            .toolbarBackground(.visible, for: .windowToolbar)
    }
}

/// Titled card (replaces GroupBox).
struct Card<Content: View>: View {
    let title: String?
    let icon: String?
    let color: Color
    let fill: Bool
    let content: Content

    init(_ title: String? = nil, icon: String? = nil, color: Color = .accentColor, fill: Bool = true,
         @ViewBuilder content: () -> Content) {
        self.title = title
        self.icon = icon
        self.color = color
        self.fill = fill
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let title {
                HStack(spacing: 8) {
                    if let icon { IconTile(symbol: icon, color: color, size: 22) }
                    Text(title).font(.headline)
                }
            }
            content
                .frame(maxWidth: fill ? CGFloat.infinity : nil, alignment: .leading)
        }
        .frame(maxWidth: fill ? CGFloat.infinity : nil, alignment: .leading)
        .cardSurface()
    }
}

/// Tinted note ("not traced yet", hints).
struct InfoBanner: View {
    let text: String
    var icon = "info.circle.fill"
    var color: Color = .blue

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: icon).foregroundColor(color).font(.system(size: 15))
            Text(text).fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 8, style: .continuous).fill(color.opacity(0.10)))
        .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous).strokeBorder(color.opacity(0.30)))
    }
}

/// Small capsule with an icon ("USB", "S/N ...").
struct Chip: View {
    let icon: String
    let text: String
    var color: Color = .gray

    var body: some View {
        Label(text, systemImage: icon)
            .font(.caption.weight(.medium))
            .foregroundColor(color)
            .padding(.horizontal, 8).padding(.vertical, 3)
            .background(Capsule().fill(color.opacity(0.14)))
    }
}

/// Colored dot + text in a capsule (printer / queue status).
struct StatusPill: View {
    let text: String
    let color: Color

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(color).frame(width: 8, height: 8)
            Text(text).font(.callout.weight(.medium))
        }
        .padding(.horizontal, 10).padding(.vertical, 5)
        .background(Capsule().fill(color.opacity(0.15)))
        .overlay(Capsule().strokeBorder(color.opacity(0.35)))
    }
}

/// Sidebar row with a colored tile.
struct TileLabel: View {
    let title: String
    let icon: String
    let color: Color

    var body: some View {
        Label { Text(title) } icon: { IconTile(symbol: icon, color: color, size: 22) }
            .padding(.vertical, 2)
    }
}

/// Big icon + title + subtitle at the top of a page.
struct PageHeader: View {
    let title: String
    let subtitle: String
    let icon: String
    let color: Color

    var body: some View {
        HStack(spacing: 12) {
            IconTile(symbol: icon, color: color, size: 38)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.title2.weight(.bold))
                Text(subtitle).foregroundColor(.secondary)
            }
        }
    }
}
