# Changelog

## Unreleased

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
  while the question was up. The question stays the same if reminders are
  reopened while it is on screen.
- Deleting a list keeps reminders moved out in another window, and deleting
  completed reminders keeps a parent if a subtask still needs it. Undo keeps
  independent edits from other writers.
- Several commands can start a new database or upgrade an old one together.
  First reminders share one default list, and two completions of the same
  repeat leave one entry in Completed.
- Import checks the backup before saving it. Completion times and pending
  snoozes travel with a backup, and large backups restore faster. Older
  backups keep untitled reminders with details and the saved dates of
  repeating reminders, including series that have ended. A dateless series
  gets a starting day if its end still allows one. Import reports empty
  reminders it skips.
- Import checks dates, times, snooze offsets, list and section IDs, and
  section references before changing any lists or reminders.

### Alerts

- Editing a reminder's notes, title, or anything but its date, time, or
  early alert no longer silences an alert that is due. Nudge keeps when each
  reminder was last given its moment, apart from when it was last edited.
- Snoozing an early alert covers the due time if it falls within the snooze.
  An early alert set in days or weeks for an all-day reminder stays at 09:00
  when the clocks change. A group of early alerts says what is coming up.
- **Open** on an alert shows its reminder, including alerts sent while the
  window was closed. If the reminder has since been deleted, it brings the
  existing window forward.
- Early and due alerts already delivered stay quiet after a time zone
  change. Editing the early lead while a due alert is being sent keeps
  that delivery recorded. Older databases use the saved alert time to check
  which lead was delivered.
- Queued alerts check their reminders again before delivery. An expired
  snooze can be snoozed again after a temporary database lock.

### Privacy

- Nudge's own data folder is made readable by its owner alone, and so are the
  database, its journal files, and backups written by `nudge export`. Files
  an earlier Nudge made readable by others are made private when opened.
- An existing folder picked with `NUDGE_DB` keeps its permissions. Export
  refuses the database and its journals as destinations, and taking a
  screenshot keeps any sample typing separate from saved reminders. A failed
  screenshot preserves the existing file at its destination.

### Commands

- Commands read times in the desktop's language, as the window does, so
  `5:30` means the same in `nudge add` as in the window.
- `nudge list today --list Home` lists what is due today in Home. `--list`,
  `--tag`, and `--search` narrow a named view instead of replacing it.
- List names are matched exactly when there is an exact match. Search takes a
  tag as it is shown, for example `#health`, and `--literal` keeps the spaces
  in a title. A failed skill install keeps the existing skill where it was.
- Completing a parent and subtask together leaves the next occurrence and
  its checklist open in either argument order. A conflicting change to the
  selected family cancels the batch. A multi-ID deletion also stops if a
  selected reminder disappeared while the command was waiting.
- Deleting a list by name respects its spacing, including repeated spaces.
- The window and CLI recognize a background service enabled for the current
  session. A failed status check suppresses the window’s enable prompt and
  makes `nudge background` print the error and exit 1.
- `nudge open --screenshot --reminder` reports an error when the requested
  reminder cannot be selected.

### Dates and repeats

- Typed dates understand the desktop's numeric date order, four-digit years,
  dayparts such as `tomorrow morning at 6`, and end dates such as
  `every 2 weeks until 2027-06-01`. Time ranges and unsupported monthly
  rules stay in the title. Write `on 12/24` to distinguish a short date
  from a fraction or a version number.
- Weekday repeats start on a day their rule includes. Completed occurrences
  stay in Completed when their parent moves forward. A repeating subtask
  carries on in its list when its one-off parent is completed.
- A time entered on its own can fall in the second occurrence of a repeated
  hour. Reapplying its date or time with `nudge edit` keeps that instant.
  A weekday, written date, or relative date followed by `morning at 6` uses
  06:00, including `every Friday morning at 6`. `Tomorrow night at midnight`
  uses the end of tomorrow. Clock ranges and weekday ranges with dayparts
  stay together in the title.
- Editing a repeat's end date keeps its original day of the month or year.
  Giving the series a new start day changes its anchor. The window and
  command line refuse a due date past the end of a repeat.

### Window

- A reminder stays in view while its title is being edited. An empty draft
  appears only in the window where it is being written. Cancelling the draft
  removes it, even if the view gave it a date or flag. After a crash, Nudge
  clears old empty drafts and recovers notes or links already saved in them.
- The editable Date and Ends fields include the year. A language change keeps
  the selected repeat, early alert, and priority. The list deletion question
  groups digits in large counts.
- Moving a reminder into the next section takes its subtasks with it.
  Undo can restore a list's sort order after a command adds a reminder.
- A language change preserves a pending Date, Time, or Ends edit and its
  original date or clock format. A newer occurrence or an explicit date
  choice replaces pending text for the old occurrence.
- A draft title rejected by the repeat end date stays focused for correction.
- The list editor refuses duplicate names. Undo and Redo retire a rename if
  another list has claimed its name. Commands explain how to rename an
  existing pair.

### Accessibility

- A reminder's row, its completion checkbox, and the calendar's days are
  exposed to screen readers, with names, states, and actions.
- Use the arrow keys and Page Up or Page Down to move in the calendar, and
  Return to choose a date.

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
