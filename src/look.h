#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

#include "theme.h"

// How Omarchy looks right now, so Nudge can look the same: the whole palette
// of the current theme (colors.toml), the control states and type size the
// Omarchy shell draws its own panels with (the theme's shell.toml, under the
// person's own), and Hyprland's window border and whether it animates, which
// Omarchy's animations toggle also says. All of it is followed live.
//
// Theme (theme.h) keeps supplying the accent and the corner rounding; this
// adds everything else a window needs to sit among Omarchy's own.
class Look : public QObject {
    Q_OBJECT
    // Colors by role: background, panel, raised, foreground, bright, dim,
    // faint, hairline, accent, accentInk, danger, the control states
    // (controlFill, hoverFill, selectedFill, pressedFill, selectionFill,
    // controlBorder, hoverBorder, focusBorder), and the surfaces of dialogs
    // and menus (cardBackground, cardBorder, scrim, menuBackground,
    // menuBorder, menuText, menuSelectedFill, menuSelectedText).
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
    // The theme's named colors, for the things a person colors themselves:
    // red, orange, yellow, green, cyan, blue, magenta, brown.
    Q_PROPERTY(QVariantMap swatches READ swatches NOTIFY changed)
    Q_PROPERTY(QStringList swatchNames READ swatchNames CONSTANT)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(QString fontFamily READ fontFamily CONSTANT)
    Q_PROPERTY(int fontSize READ fontSize NOTIFY changed)
    Q_PROPERTY(int controlBorderWidth READ controlBorderWidth NOTIFY changed)
    Q_PROPERTY(int windowBorderWidth READ windowBorderWidth NOTIFY changed)
    Q_PROPERTY(bool reduceMotion READ reduceMotion NOTIFY changed)

public:
    // The person's own settings are read under home, as Omarchy keeps them;
    // a current directory given without one is a theme on its own.
    explicit Look(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {},
                  const QString &hyprlandEvents = {}, const QString &home = {});
    ~Look() override;

    // The folder of the Omarchy theme a picture asks for (nudge open
    // --theme), or nothing when there is none. A name is one of the themes,
    // the person's own before those Omarchy comes with, and never a way out
    // of them; anything else is a path. A test says where themes are kept.
    static QString themeFolder(const QString &asked, const QStringList &folders = {});

    QVariantMap colors() const { return m_colors; }
    QVariantMap swatches() const { return m_swatches; }
    QStringList swatchNames() const;
    bool dark() const { return m_dark; }
    // The fontconfig alias `omarchy font set` points at the Omarchy font.
    QString fontFamily() const { return QStringLiteral("monospace"); }
    int fontSize() const { return m_fontSize; }
    int controlBorderWidth() const { return m_controlBorderWidth; }
    int windowBorderWidth() const { return m_windowBorderWidth; }
    bool reduceMotion() const { return m_animationsOff || m_hyprlandStill; }

    QColor color(const QString &role) const { return m_colors.value(role).value<QColor>(); }

    // A TOML file as "section.key" -> value; keys before any section have no
    // prefix. Enough of TOML for theme files: strings, numbers, comments.
    static QHash<QString, QString> readToml(const QString &path);
    static QColor mix(const QColor &base, const QColor &ink, double amount);
    static QColor contrastInk(const QColor &color);

signals:
    void changed();

private:
    void reload();
    void askHyprland();

    QString m_directory;
    QString m_toggles, m_ownShell;
    QVariantMap m_colors, m_swatches;
    bool m_dark = true;
    int m_fontSize = 12;
    int m_controlBorderWidth = 1;
    int m_windowBorderWidth = 2;
    bool m_animationsOff = false; // Omarchy's toggle
    bool m_hyprlandStill = false; // Hyprland's own answer

    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    QProcess m_borderProcess, m_animationsProcess;
    bool m_askAgain = false;
    HyprlandReloads m_reloads;
};
