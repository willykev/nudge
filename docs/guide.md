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
- **Typed dates.** In `Call the dentist tomorrow at 9 #health !!`, Nudge
  reads the title, date, time, tag, and priority as you type. Search for
  `#health` to find reminders with that tag.
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

## Set a date or repeat

Type a date in a new reminder's title or press `D` to set it on its own.
Numeric dates follow the desktop's date order. In a title, write `on 12/24`
or `on 24.12.` to tell Nudge the number is a date. Use `2026-12-24` for a date
you can paste in any language. A named date in a title can take a year from
1900 through 2199 directly. For other years use a comma, as in `Dec 1, 1200`,
or write the date out as `1200-12-01`. The editable Date and Ends fields
include the year so you can copy a date and paste it back. If you change the
desktop language while typing, Return reads your entry in the language you
started with. Choosing **Today** or another date replaces the Date field’s
pending text. In the calendar, use the arrow keys to move a day or week,
Page Up or Page Down to change months, and Return to pick a date.

For a time without am or pm, Nudge reads 1 to 7 as afternoon, 8 to 11 as
morning, and 12 as noon. On a 12-hour clock that makes `5:30` 17:30.
Type `5:30am` or `05:30` for an early time, or write
`Friday morning at 6`. A time entered without a date uses the next time
the clock shows it, including the second time when clocks go back.
On their own, morning, afternoon, evening, and night after a date mean
09:00, 15:00, 18:00, and 21:00 respectively. In `tomorrow night at 12`,
you can also write `midnight` or `12:00am` for midnight at the end of tomorrow.

Write `on Sunday` or `at midnight` to make the date or time clear in a title.
Ranges such as `from 5 pm to 6 pm` and rules such as `every second Tuesday`
stay in the title. `This Friday` includes today when today is Friday.

A repeat on weekdays starts on Monday if its date falls on the weekend.
With an explicit weekday, `every Friday` starts on the first Friday on or
after the given start date. Type `every 2 weeks until 2027-06-01` to set an
end date. The due date can fall on the end date or earlier. If the day is absent
from a month or year, a monthly or yearly reminder uses the last day of
that month and keeps its original day for later occurrences, including
when you edit the end date. To pick a new day, give the series a new start,
for example `nudge edit 12 --repeat "every month on 2031-03-15"`.

Completing a repeat leaves a completed occurrence and moves the reminder
forward. If you complete a one-off reminder that has a repeating subtask,
the subtask's next occurrence becomes a reminder of its own in the same list
and section. You can name both IDs in one `nudge done` command. Each current
occurrence is completed once, in either argument order. For a repeating
parent, the next checklist opens with its ordinary subtasks unchecked. If a
selected family member moves on while the command is waiting, the batch
stops so you can check the current occurrence.

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
| `Alt+↑` / `Alt+↓` | Move up or down. Crossing a section takes the reminder's subtasks with it. |
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

While you rename a reminder, its row stays in view. Finish the title before
scrolling. To move reminders by hand, select **Manual Order** in the sort menu.
You can undo a change of sort order even after a command adds a reminder to the list.

## Reminders while the window is closed

Alerts arrive while Nudge is open. To have them arrive when it is closed too,
press `B` in the window or run `nudge background on`. That enables a small
user service (`nudge.service`, which runs `nudge daemon`); `nudge background
off` disables it again. After a sleep, whatever came due while the lid was
closed is announced as the machine wakes.

Choose **In an Hour** or **Tomorrow** to postpone an alert, including an
early alert. If the due time falls before the snooze ends, it is covered by
the snoozed alert. **Open** shows the reminder in the window.

An all-day reminder alerts at 09:00. An early alert set a day or week ahead
of it also arrives at 09:00 local time, even when the clocks change. Pending
alerts follow the local time zone. An alert already delivered stays delivered
when you change zones. For a reminder with a time, the lead measures elapsed time.

## From the command line

Every command works without a display, takes `--json`, exits 0 or 1, and
writes errors to stderr. The window shows a change at once if it is open.

```sh
nudge add "Water the plants every friday"
nudge add "Send the report" --list Work --due "next friday" --time 15:00 --priority high
nudge list today
nudge list --list Work --json
nudge list today --list Work
nudge done 12
nudge edit 12 --due tomorrow
nudge lists add Groceries
nudge export backup.json
```

`nudge help` lists the commands and `nudge help add` a command's options.
`nudge skill install` installs a short skill so coding agents can keep your
reminders for you.

Use `nudge add --literal "Send  the report"` to save the title as written,
including its spacing. For a repeating reminder, `nudge done` returns the
ID of the reminder that moved forward. The entry in Completed has its own ID
and is the one to pass to `nudge reopen`.

Give each list a distinct name. If a command finds two lists with the same
name in an older database, select one in the sidebar and press Return to rename it.
To capture a specific reminder with its details, use
`nudge open --reminder 12 --details --screenshot reminder.png`. If the
reminder is missing or the image cannot be written, the command exits 1 and
preserves any existing output file.

`nudge background --json` reports `false` for a disabled service. If the
service status cannot be read, the command writes the error to stderr and exits 1.

## Where things are

Reminders live in `~/.local/share/nudge/nudge.db`, a SQLite file written
through on every change. The default folder, the database, and the backups
`nudge export` writes are readable by you alone. Setting `NUDGE_DB` points
Nudge at another file. An existing folder chosen that way keeps its permissions.

Commands read dates and times in the language picked for the desktop, as the
window does, so `5:30` is the same time typed in either.

### Back up and restore

`nudge export backup.json` saves a JSON backup. Choose a path separate from the
open database and its journal files. `nudge import backup.json` reads it back
with its lists, sections, subtasks, completion times, and pending snoozes.
An older backup can contain a reminder with details and a blank title.
It comes back as **Untitled reminder**. Import reports how many empty
reminders it skipped. Repeats keep their saved dates, even after the series
has ended. A dateless repeat starts today, or on Monday for a weekday rule
restored over the weekend. If that start would fall after its end, it stays
dateless. Each saved snooze needs a time and a UTC offset or `Z`.
Invalid dates, duplicate list or section IDs, and references to the wrong
section cause the whole import to fail before it changes your reminders.

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
