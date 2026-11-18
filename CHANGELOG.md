# Changelog

## Unreleased

Fixes from a review of 0.1.3.

### Saving

- A window that has not caught up with another window or a command no
  longer writes over what they changed: it saves only what it changed
  itself. A reminder, list, or section deleted elsewhere is not made again
  by a stale window; the change is refused, and the window says so.
- A backup keeps lists and sections apart that have the same name (`Home`
  and `HOME`, or two `Later` sections). Exports now carry each one's id, and
  older exports still import. Imported lists are matched to existing ones
  only once each.
- Delete Completed deletes the reminders it asked about, not ones completed
  while the question was up.

### Alerts

- Editing a reminder's notes, title, or anything but its date, time, or
  early alert no longer silences an alert that is due. Nudge keeps when each
  reminder was last given its moment, apart from when it was last edited.

### Privacy

- The data folder is made readable by its owner alone, and so are the
  database, its journal files, and backups written by `nudge export`. Files
  an earlier Nudge made readable by others are made private when opened.

### Commands

- Commands read times in the desktop's language, as the window does, so
  `5:30` means the same in `nudge add` as in the window.
- `nudge list today --list Home` lists what is due today in Home. `--list`,
  `--tag`, and `--search` narrow a named view instead of replacing it.

### Accessibility

- A reminder's row, its completion checkbox, and the calendar's days are
  exposed to screen readers, with names, states, and actions.

## 0.1.3 — 2026-11-16

A new README, with pictures drawn by Nudge itself.

- The README shows Nudge at work: a list with its details, a reminder being
  typed as Nudge reads its date, the Scheduled view, the calendar, and one
  list in four themes. It adds a short tour, measured performance, and how
  to update and remove Nudge.
- `nudge open --screenshot` takes a window size (`--size`), an Omarchy theme
  (`--theme`), a time to take it to be (`--now`), and words for a new
  reminder to start with (`--type`), so a picture shows the same wherever
  it is taken.
- `bin/screenshots` redraws the README's pictures from the reminders in
  `examples/week.json`, off screen.
- The guide says how to update and remove Nudge and what to check when an
  alert does not arrive, and leaves building to `CONTRIBUTING.md`.

## 0.1.2 — 2026-11-14

- Renaming a reminder that is scrolled out of view no longer stops as soon
  as it starts, which sent the next keys typed to the single-key shortcuts.

## 0.1.1 — 2026-11-13

Fixes from a full code and security review of 0.1.0.

### Security

- Reminder text is always shown as plain text. Tags, link labels, list names,
  and messages from an import or another program can no longer load remote
  images or restyle the window.
- The command line shows control characters in reminder text harmlessly, so
  an imported reminder cannot write to the clipboard, hide a link, or rewrite
  what the terminal shows.
- Builds no longer run qmake files left above the source tree, such as one
  another user puts in /tmp, and releases are built inside `dist/`.
- A link that is not a web link shows its scheme, so it never looks like one.

### Fixes

- Typing in the details pane always lands on the reminder it was typed for,
  including a new reminder and the repeat interval, and the dropdowns no
  longer change reminders from the keyboard after a click.
- The title being typed is kept when you click elsewhere or another window
  takes the keyboard, and single-key shortcuts no longer fire while you type.
- Ctrl+Shift+Z redoes again.
- Adding a reminder and naming it are one step to undo. An edit after an undo
  starts a new step, changes from other programs no longer clear the undo
  history, and undo no longer brings back an alert that already went off.
- Changes from other programs are always picked up. A save or delete that
  fails says so and changes nothing.
- A completed reminder never hides an open subtask.
- Delete Completed outside a list deletes only the reminders that view holds.
- Move to Section offers only the list's own sections, and the sort menu
  follows undo.
- A tag written in different cases is one tag in the sidebar.
- Quick entry: "tonight at 9" is 21:00, and "tonight" after eight is the next
  hour. On a 12-hour clock "5:30" is read the way "5" is. "every friday at 9"
  typed after nine on a Friday starts next week. "this evening" is understood,
  "room 2a" and words like "this" and "on" stay in the title, and "2/29" works.
- Completing an overdue repeating reminder with a time moves it to the next
  time still ahead.
- Alert buttons act only on the moment they announced, alert text with `&` or
  `<` shows as written, and alerts left behind when Nudge quits lose their
  buttons.
- Clicking a background alert with no window open shows the window, and
  turning background reminders off no longer closes it.
- The window and the background service shut down cleanly when stopped or on
  logout, keeping a title being typed.
- The command line follows the window's rules for times, dates, repeats, and
  tags. It refuses to move a subtask out of its parent's list, acts once on an
  id given twice, names every reminder it deletes, leaves existing lists alone
  on import, reports writes that fail, and `export` and `skill` take `--json`.
- Plural forms read correctly in languages Nudge has no translation for, and
  missing translations were added.
- Nudge follows Omarchy's animations toggle and your own text size, no longer
  asks Hyprland for its settings every second, and a read-only command no
  longer makes open windows reload.

### Release tooling

- `bin/release` checks everything before it pushes, builds the downloadable
  `PKGBUILD` itself, and compares existing release assets by checksum.
- Changing `VERSION` rebuilds the version into an existing build.

## 0.1.0 — 2026-11-13

First public release.

- Lists, sections, subtasks, tags, flags, and priorities.
- Due dates, repeating reminders, and natural-language quick entry.
- Desktop alerts and an optional background reminder service.
- Keyboard controls, undo/redo, local SQLite storage, and JSON import/export.
- Live Omarchy theming, nine interface languages, and a command-line interface.
