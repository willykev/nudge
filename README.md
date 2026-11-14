![Nudge with sample reminders, subtasks, tags, and a due-date editor](screenshot.png)

# Nudge

A reminders app for Omarchy. Keep your lists in order, plan your day, and get a
nudge when something is due.

- Organize reminders into lists, sections, and subtasks.
- Set due dates, repeating reminders, priorities, flags, and tags.
- See what needs attention in Today, Scheduled, and Flagged.
- Follow your Omarchy theme automatically, with keyboard shortcuts throughout.

Everything stays on your computer. No account or cloud service is required.

## Install

From the project folder on Omarchy or Arch Linux:

```sh
bin/install
```

This builds and installs the app. Open **Nudge** from the app launcher, or run
`nudge open`.

Building requires a C++ compiler, `make`, `qmake6`, and the `qt6-base`,
`qt6-declarative`, `qt6-svg`, and `qt6-wayland` packages. The Arch package is
named `nudge-reminders`; the command is `nudge`.

## Make your first reminder

Press **N**, type `Call the dentist tomorrow at 9 #health`, and press **Enter**.
Nudge picks out the date, time, and tag. Press **I** to adjust the details.

| Key | Action |
| --- | --- |
| `N` | Add a reminder |
| `Shift+N` | Add a list |
| `Space` | Complete or reopen |
| `I` | Show details |
| `F` | Flag a reminder |
| `/` | Search |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo |
| `?` | Show every shortcut |

To receive reminders while the window is closed, press **B** or run:

```sh
nudge background on
```

## Use the terminal

```sh
nudge add "Water the plants every friday"
nudge list today
nudge export backup.json
```

Run `nudge help` for all commands. Add `--json` for scripts, or run
`nudge skill install` to install the bundled agent instructions.

## Your data

Reminders live in `~/.local/share/nudge/nudge.db`. Use `nudge export` and
`nudge import` for backups. Lists are local to this machine; device sync is
not included.

## Development

```sh
bin/build     # Build into build/nudge
bin/test      # Run the tests without opening a window
```

See the [full guide](docs/guide.md) for all shortcuts, features, and language
support.

[MIT license](LICENSE), with acknowledgments to the upstream projects.
