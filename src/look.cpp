// Reading the whole palette follows Hype's AppTheme, and the roles and state
// tokens follow the Omarchy shell's Color and Style (both MIT, David
// Heinemeier Hansson).
#include "look.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>
#include <functional>

namespace {

QColor withAlpha(QColor color, double alpha) {
    color.setAlphaF(qBound(0.0, alpha, 1.0) * color.alphaF());
    return color;
}

// A color as theme files write one: "#1a1b26", or Hyprland's "rgb(1a1b26)"
// and "rgba(1a1b26ee)".
QColor parseColor(const QString &text) {
    static const QRegularExpression hyprland("^rgba?\\(([0-9a-fA-F]{6})([0-9a-fA-F]{2})?\\)$");
    const auto match = hyprland.match(text);
    if (match.hasMatch()) {
        QColor color("#" + match.captured(1));
        if (!match.captured(2).isEmpty())
            color.setAlpha(match.captured(2).toInt(nullptr, 16));
        return color;
    }
    return text.startsWith('#') ? QColor(text) : QColor();
}

} // namespace

QColor Look::mix(const QColor &base, const QColor &ink, double amount) {
    return QColor::fromRgbF(base.redF() + (ink.redF() - base.redF()) * amount,
                            base.greenF() + (ink.greenF() - base.greenF()) * amount,
                            base.blueF() + (ink.blueF() - base.blueF()) * amount);
}

// Black or white, whichever reads on the color (WCAG relative luminance).
QColor Look::contrastInk(const QColor &color) {
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    const double luminance =
        .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
    return luminance > .179 ? QColor("#111111") : QColor("#ffffff");
}

QHash<QString, QString> Look::readToml(const QString &path) {
    QHash<QString, QString> values;
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return values;
    static const QRegularExpression section("^\\s*\\[([A-Za-z0-9_.-]+)\\]");
    // A quoted value may hold a #; a bare one is a color or ends at a space
    // or a comment.
    static const QRegularExpression entry(
        R"re(^\s*([A-Za-z0-9_-]+)\s*=\s*(?:"([^"]*)"|'([^']*)'|(#[0-9A-Fa-f]{3,8}|[^#\s]+)))re");
    QString prefix;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine());
        const auto header = section.match(line);
        if (header.hasMatch()) {
            prefix = header.captured(1) + ".";
            continue;
        }
        const auto match = entry.match(line);
        if (!match.hasMatch())
            continue;
        const QString value = !match.captured(2).isNull()   ? match.captured(2)
                              : !match.captured(3).isNull() ? match.captured(3)
                                                            : match.captured(4);
        values.insert(prefix + match.captured(1), value);
    }
    return values;
}

QString Look::themeFolder(const QString &asked, const QStringList &folders) {
    if (asked.isEmpty())
        return {};
    QStringList candidates;
    if (!asked.contains('/') && asked != "." && asked != "..") {
        const QStringList kept = !folders.isEmpty() ? folders
                                                    : QStringList{QDir::homePath() + "/.config/omarchy/themes",
                                                                  "/usr/share/omarchy/themes"};
        for (const QString &folder : kept)
            candidates << folder + "/" + asked;
    }
    candidates << asked;
    for (const QString &candidate : std::as_const(candidates))
        if (QFile::exists(candidate + "/colors.toml"))
            return QDir::cleanPath(QDir::current().absoluteFilePath(candidate));
    return {};
}

Look::Look(const QString &directory, QObject *parent, const QString &hyprctl, const QString &hyprlandEvents,
           const QString &home)
    : QObject(parent), m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Look::reload);

    // The person's own settings: the flags Omarchy's toggles leave, and the
    // shell.toml the shell lays over the theme's, where `omarchy display
    // text size` keeps its size.
    const QString settings = !home.isEmpty() ? home : directory.isEmpty() ? QDir::homePath() : QString();
    if (!settings.isEmpty()) {
        m_toggles = settings + "/.local/state/omarchy/toggles/hypr";
        m_ownShell = settings + "/.config/omarchy/shell.toml";
    }

    // Hyprland is asked, rather than its config read, as for the rounding in
    // Theme: themes and the person's own overrides can both set these. It is
    // asked again each time it reloads, as Theme does.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
                : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                    ? QStandardPaths::findExecutable("hyprctl")
                    : QString();
    connect(&m_borderProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const auto value = QJsonDocument::fromJson(m_borderProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_windowBorderWidth) {
            m_windowBorderWidth = value.toInt();
            emit changed();
        }
    });
    connect(&m_animationsProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const auto reply = QJsonDocument::fromJson(m_animationsProcess.readAllStandardOutput()).object();
        if (code != 0 || status != QProcess::NormalExit || (!reply.contains("bool") && !reply.contains("int")))
            return;
        // `omarchy toggle animations` turns them off for machines that draw
        // on the CPU; a window that still animated would be the odd one out.
        const bool animates = reply.contains("bool") ? reply.value("bool").toBool() : reply.value("int").toInt() != 0;
        if (m_hyprlandStill == animates) {
            m_hyprlandStill = !animates;
            emit changed();
        }
    });
    for (QProcess *process : {&m_borderProcess, &m_animationsProcess})
        connect(process, &QProcess::finished, this, [this] {
            if (m_askAgain && m_borderProcess.state() == QProcess::NotRunning &&
                m_animationsProcess.state() == QProcess::NotRunning) {
                m_askAgain = false;
                QTimer::singleShot(0, this, &Look::askHyprland);
            }
        });
    connect(&m_reloads, &HyprlandReloads::reloaded, this, &Look::askHyprland);
    m_reloads.listen(!hyprlandEvents.isEmpty() ? hyprlandEvents
                     : directory.isEmpty()     ? HyprlandReloads::socket()
                                               : QString());
    askHyprland();
    reload();
}

Look::~Look() {
    for (QProcess *process : {&m_borderProcess, &m_animationsProcess})
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
}

QStringList Look::swatchNames() const {
    return {"red", "orange", "yellow", "green", "cyan", "blue", "magenta", "brown"};
}

void Look::askHyprland() {
    if (m_hyprctl.isEmpty())
        return;
    // Answers on their way may predate what changed, so ask once more when
    // both are in.
    if (m_borderProcess.state() != QProcess::NotRunning || m_animationsProcess.state() != QProcess::NotRunning) {
        m_askAgain = true;
        return;
    }
    m_borderProcess.start(m_hyprctl, {"-j", "getoption", "general:border_size"});
    m_animationsProcess.start(m_hyprctl, {"-j", "getoption", "animations:enabled"});
}

void Look::reload() {
    const QString theme = m_directory + "/theme";
    const QHash<QString, QString> palette = readToml(theme + "/colors.toml");
    // The person's own shell.toml wins key by key over the theme's, so it
    // outlives a theme switch, as in the shell.
    QHash<QString, QString> shell = readToml(theme + "/shell.toml");
    shell.insert(readToml(m_ownShell));
    // The flag `omarchy toggle animations` leaves says what Hyprland will
    // once it has reloaded, and says it without Hyprland too.
    const bool animationsOff = !m_toggles.isEmpty() && QFileInfo(m_toggles + "/no-animations.lua").isFile();

    // Without Omarchy the window still has a look: Tokyo Night, its default.
    auto named = [&palette](const QString &key, const QColor &fallback) {
        const QColor color = parseColor(palette.value(key));
        return color.isValid() ? color : fallback;
    };
    const QColor bg = named("background", "#1a1b26");
    const QColor fg = named("foreground", "#a9b1d6");
    const QColor accent = named("accent", "#7aa2f7");
    const QColor red = named("red", "#f7768e");
    const QColor yellow = named("yellow", "#e0af68");
    const QString mode = palette.value("mode");
    const bool dark = mode == "light" ? false : mode == "dark" ? true : contrastInk(bg) == QColor("#ffffff");

    // A token in shell.toml is a color, a role, a pointer to another key
    // ("hyprland.active-border"), or a gradient, of which the first stop serves.
    std::function<QColor(const QString &, const QColor &, int)> resolve = [&](const QString &token, const QColor &fallback,
                                                                             int depth) -> QColor {
        static const QRegularExpression space("\\s+");
        QString first;
        for (const QString &part : token.trimmed().split(space, Qt::SkipEmptyParts))
            if (!part.endsWith("deg")) {
                first = part;
                break;
            }
        const QString role = first.toLower();
        if (role.isEmpty())
            return fallback;
        if (role == "foreground" || role == "text")
            return fg;
        if (role == "background")
            return bg;
        if (role == "accent")
            return accent;
        if (role == "urgent")
            return red;
        if (role == "muted")
            return named("muted", mix(bg, fg, .3));
        if (role == "transparent")
            return QColor(0, 0, 0, 0);
        if (shell.contains(role) && depth < 4)
            return resolve(shell.value(role), fallback, depth + 1);
        const QColor color = parseColor(first);
        return color.isValid() ? color : fallback;
    };
    auto token = [&](const QString &key, const QColor &fallback) {
        return shell.contains(key) ? resolve(shell.value(key), fallback, 0) : fallback;
    };
    auto number = [&shell](const QString &key, double fallback) {
        bool ok = false;
        const double value = shell.value(key).toDouble(&ok);
        return ok ? value : fallback;
    };

    // The states every control in the shell's own kit shares.
    const QColor normal = token("controls.normal-color", fg);
    const QColor hover = token("controls.hover-cursor-color", fg);
    const QColor focus = token("controls.focus-color", hover);
    const QColor selected = token("controls.selected-color", fg);
    const double hoverFill = number("controls.hover-cursor-fill-alpha", .08);
    const double hoverBorder = number("controls.hover-cursor-border-alpha", .25);

    const QVariantMap colors{
        {"background", bg},
        {"panel", named("dark_background", mix(bg, dark ? QColor("#000000") : fg, dark ? .25 : .05))},
        {"raised", named("lighter_background", mix(bg, fg, .08))},
        {"foreground", fg},
        {"bright", named("bright_foreground", fg)},
        // Derived rather than taken from dark_foreground, which some themes
        // make too pale to read as text.
        {"dim", mix(bg, fg, .62)},
        {"faint", mix(bg, fg, .4)},
        {"hairline", mix(bg, fg, .14)},
        {"accent", accent},
        {"accentInk", contrastInk(accent)},
        {"danger", red},
        {"controlFill", withAlpha(normal, number("controls.normal-fill-alpha", .04))},
        {"hoverFill", withAlpha(hover, hoverFill)},
        {"selectedFill", withAlpha(selected, number("controls.selected-fill-alpha", .18))},
        {"pressedFill", withAlpha(token("controls.pressed-color", hover), number("controls.pressed-fill-alpha", .22))},
        {"selectionFill", withAlpha(token("controls.selection-color", fg), number("controls.selection-fill-alpha", .35))},
        {"controlBorder", withAlpha(token("controls.normal-border", normal), number("controls.normal-border-alpha", .4))},
        {"hoverBorder", withAlpha(token("controls.hover-cursor-border", hover), hoverBorder)},
        {"focusBorder", withAlpha(token("controls.focus-border", focus), number("controls.focus-border-alpha", hoverBorder))},
        {"focusFill", withAlpha(focus, number("controls.focus-fill-alpha", hoverFill))},
        {"cardBackground", withAlpha(token("popups.background", bg), number("popups.background-alpha", 1))},
        {"cardText", token("popups.text", fg)},
        {"cardBorder", withAlpha(token("popups.border", accent), number("popups.border-alpha", 1))},
        {"scrim", withAlpha(token("menu.scrim", bg), number("menu.scrim-alpha", .5))},
        {"menuBackground", withAlpha(token("menu.background", bg), number("menu.background-alpha", 1))},
        {"menuText", token("menu.text", fg)},
        {"menuBorder", withAlpha(token("menu.border", fg), number("menu.border-alpha", 1))},
        {"menuSelectedFill",
         withAlpha(token("menu.selected-background", fg), number("menu.selected-background-alpha", .08))},
        {"menuSelectedText", token("menu.selected-text", accent)},
    };
    const QVariantMap swatches{
        {"red", red},
        {"orange", named("orange", mix(red, yellow, .5))},
        {"yellow", yellow},
        {"green", named("green", "#9ece6a")},
        {"cyan", named("cyan", "#449dab")},
        {"blue", named("blue", "#7aa2f7")},
        {"magenta", named("magenta", "#ad8ee6")},
        {"brown", named("brown", mix(yellow, bg, .45))},
    };
    const int fontSize = qMax(1, int(number("font.base-size", 12)));
    const int borderWidth = qMax(0, int(std::lround(number("controls.normal-border-width", 1))));

    if (colors != m_colors || swatches != m_swatches || dark != m_dark || fontSize != m_fontSize ||
        borderWidth != m_controlBorderWidth || animationsOff != m_animationsOff) {
        m_colors = colors;
        m_swatches = swatches;
        m_dark = dark;
        m_fontSize = fontSize;
        m_controlBorderWidth = borderWidth;
        m_animationsOff = animationsOff;
        emit changed();
    }

    // A theme switch replaces symlinks and files, so watch the parents too
    // and re-arm after every change, as Theme does. What is not there yet is
    // watched for through its nearest existing ancestor.
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    auto nearest = [](QString path) {
        while (!QFileInfo::exists(path) && path != "/")
            path = QFileInfo(path).absolutePath();
        return path;
    };
    const QString ancestor = nearest(m_directory);
    QStringList candidates{ancestor,          QFileInfo(ancestor).absolutePath(), m_directory, theme,
                           theme + "/colors.toml", theme + "/shell.toml"};
    // Toggles drop flags in their directory, and `omarchy display text size`
    // writes a new shell.toml and moves it over the old.
    if (!m_toggles.isEmpty())
        candidates << nearest(m_toggles) << nearest(QFileInfo(m_ownShell).absolutePath()) << m_ownShell;
    candidates.removeDuplicates();
    for (const QString &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);
}
