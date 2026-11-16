# Nudge guide

Reminders for the Omarchy desktop: lists, due dates and times, repeats,
subtasks, tags, priorities, flags, and a nudge when something is due. It is
driven from the keyboard, looks like the rest of Omarchy whatever theme is
on, and keeps everything in one local file.

## What it does

- **Lists** in your own colors, with **sections** inside them and **subtasks**
  under any reminder.
- **Today, Scheduled, All, Flagged, Completed**, and one view per **tag**,
  gathered from every list. **Search** looks through titles, notes, links,
  and tags.
- **Due dates and times**, an **early reminder** ahead of them, and **repeats**:
  daily, weekdays, weekly, monthly, yearly, or every few of those, with an
  end date. Completing a repeating reminder moves it to its next date and
  keeps what was done in Completed.
- **Typed dates.** A new reminder reads its own date as you type:
  `Call the dentist tomorrow at 9 #health !!` is a title, a date, a time, a
  tag, and a priority. The date field takes the same words. A time without am
  or pm is a guess: an hour from 1 to 7 is the afternoon (`at 5` is 17:00, and
  on a 12-hour clock `5:30` is 17:30), 8 to 11 the morning, and 12 noon. Type
  `5:30am` or `05:30` for an early time.
- **Alerts** as desktop notifications, with buttons to complete the reminder
  or be reminded in an hour or tomorrow. A reminder with a date but no time
  alerts at nine in the morning.
- **Undo** for everything, step by step, including deleting a whole list.
- **Sorting** per list: manual order, due date, priority, title, or date
  created.

## Looks like Omarchy

Nudge has no colors or fonts of its own. It reads the current theme and
follows it live, the way the Omarchy shell and Hype do:

- **The whole palette** from the theme's `colors.toml`: background, text,
  accent, and the named colors. A list is colored by name (blue, green,
  magenta, ...), so it changes shade when the theme does. Light themes work.
- **The shell's controls**, from the theme's `shell.toml`: fields, buttons,
  dropdowns, and menus are drawn with the same fills, hairline borders, and
  selected states as the shell's own panels. Buttons are outlines, never
  solid blocks; the switch is one color; dialogs dim the window as the
  shell's do.
- **The cursor of the Omarchy menu**: the reminder the keys are on has a
  faint fill, a hairline around it, and its title in the accent. Everything
  else is the theme's text color, with color kept for the small marks: a
  list's dot, a flag, an overdue date.
- **The Omarchy font**, the one `omarchy font set` chooses, at the shell's
  size: the theme's, or your own from `omarchy display text size`. A new size
  shows at once, a new font the next time Nudge starts.
- **Hyprland's window corners and border**: checkboxes, buttons, and dialogs
  are square where windows are square and round where they are round, and
  dialogs wear the window border. With animations switched off in Hyprland
  or by `omarchy toggle animations`, nothing in Nudge moves either.

## Install

```sh
bin/install
```

This builds Nudge and installs it as an Arch package (`makepkg -fsi`), so it
shows up in the Omarchy app launcher (`Super + Space`). It needs `qmake6`,
`make`, a C++ compiler, and Qt's base, declarative, SVG, and Wayland modules,
all of which are part of Omarchy. The package is named `nudge-reminders`; the
command is `nudge`.

To update, run `git pull` and `bin/install` again. To remove Nudge, run
`nudge background off` and then `sudo pacman -R nudge-reminders`. Your
reminders stay in `~/.local/share/nudge/`; delete that folder to remove them
too.

## Shortcuts

`?` shows these in the window, and the line at its foot keeps the main ones
in sight.

| Key | Does |
| --- | --- |
| `N` | New reminder. Return saves it and starts the next. |
| `Space` | Complete or reopen |
| `Return` | Rename |
| `I` | Show or hide details |
| `F` | Flag |
| `1` `2` `3` `0` | Priority: low, medium, high, none |
| `Tab` / `Shift+Tab` | Make a subtask / back to a reminder of its own |
| `X` | Fold or unfold subtasks |
| `M` | Move to another list |
| `Alt+↑` / `Alt+↓` | Move up or down, and on into the next section |
| `O` | Open its link |
| `Delete` or `Backspace` | Delete |
| `T` / `Shift+T` / `W` | Due today / tomorrow / in a week |
| `D` / `Shift+D` | Type a date / no date |
| `↑` `↓` | Previous or next (`K` and `J` work too) |
| `←` `→` | Lists or reminders (`H` and `L` work too) |
| `G` / `Shift+G` | First or last (`Home` and `End` work too) |
| `/` | Search |
| `Shift+N` | New list. With the lists focused, Return edits one and Delete removes it. |
| `Shift+S` | New section |
| `S` | Change the sort order |
| `C` / `Shift+C` | Show or hide completed / delete completed |
| `B` | Reminders in the background, on or off |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo |
| `Esc` | Back to the reminders |
| `Q` | Quit |

## Reminders while the window is closed

Alerts arrive while Nudge is open. To have them arrive when it is closed too,
press `B` in the window or run `nudge background on`. That enables a small
user service (`nudge.service`, which runs `nudge daemon`); `nudge background
off` disables it again. After a sleep, whatever came due while the lid was
closed is announced as the machine wakes.

## From the command line

Every command works without a display, takes `--json`, exits 0 or 1, and
writes errors to stderr. The window shows a change at once if it is open.

```sh
nudge add "Water the plants every friday"
nudge add "Send the report" --list Work --due "next friday" --time 15:00 --priority high
nudge list today
nudge list --list Work --json
nudge done 12
nudge edit 12 --due tomorrow
nudge lists add Groceries
nudge export backup.json
```

`nudge help` lists the commands and `nudge help add` a command's options.
`nudge skill install` installs a short skill so coding agents can keep your
reminders for you.

## Where things are

Reminders live in `~/.local/share/nudge/nudge.db`, a SQLite file written
through on every change. `nudge export` writes everything as JSON, and `nudge
import` reads it back. Setting `NUDGE_DB` points Nudge at another file.

## What it does not do

Nudge keeps reminders on this machine. It does not sync between devices or
share lists, alert by location, attach images, or have templates, a column
view, list groups, or smart lists you define yourself. Reminders are
rearranged from the keyboard and the menu rather than by dragging, and one is
selected at a time. Typed dates are read in English; in other languages, type
a date the way the language writes it or pick one from the calendar.

The window is translated into Danish, Dutch, French, German, Italian,
Portuguese (Brazil), Spanish (Mexico), and Swedish, and is in English
otherwise.

## Troubleshooting

**No alert arrived while the window was closed.** Alerts arrive with the
window closed only once the background service is on: press `B` in the
window or run `nudge background on`. `systemctl --user status nudge.service`
says whether it is running.

**A date was not picked out of what I typed.** Typed dates are read in
English. Press `D` to type the date on its own, or pick one from the
calendar in the details.
