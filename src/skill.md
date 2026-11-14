---
name: nudge
description: |
  Add, list, change, and complete the person's reminders and to-dos in Nudge,
  the reminders app on their Omarchy desktop. Use for ANY request to remind
  them of something, to add or check off a task or to-do, to see what is due
  today or this week, or to organize their reminder lists.
---

# Nudge

Nudge keeps the person's reminders in lists, with due dates, repeats, subtasks,
tags, priorities, and flags. The `nudge` command reads and changes them; no
command needs a display, and the window updates at once if it is open.

## Commands

- Add: `nudge add "Call the dentist tomorrow at 9 #health !!"`. Dates, times,
  repeats ("every friday"), `#tags`, and `!` to `!!!` for priority are read out
  of the text. Options say the same explicitly: `--list`, `--due`, `--time`,
  `--repeat`, `--priority`, `--flag`, `--tag`, `--notes`, `--url`, `--parent <id>`
  for a subtask, and `--literal` to keep the text word for word.
- Times: one without am or pm is a guess, in the text and in `--time` alike.
  An hour from 1 to 7 is the afternoon (`at 5` is 17:00, and on a 12-hour
  clock `5:30` is 17:30), 8 to 11 the morning, and 12 noon; `tonight at 9` is
  21:00. Write `5:30am` or `05:30` for an early time.
- See: `nudge list today --json` (also `scheduled`, `all`, `flagged`,
  `completed`, or `--list <name>`, `--tag <tag>`, `--search <text>`), and
  `nudge show <id> --json` for one reminder with its subtasks.
- Change: `nudge edit <id> --due "next friday" --priority high`; `--due none`,
  `--time none`, and `--repeat none` clear a value.
- Finish: `nudge done <id>...`, and `nudge reopen <id>...` to take it back.
  Completing a repeating reminder moves it to its next date.
- Remove: `nudge delete <id>...` deletes reminders with their subtasks.
- Lists: `nudge lists --json`, `nudge lists add <name>`,
  `nudge lists delete <name>` (`--force` when it still holds reminders).
- Everything: `nudge export backup.json`, `nudge import backup.json`.

## Working with the person

- Find the id first. Ids come from `nudge list --json`; never guess one.
- Read back what was understood: `nudge add --json` prints the reminder as
  saved, so check its `due` and `time` before saying it is set.
- Deleting is theirs to decide. Complete a reminder rather than delete it
  unless they asked for it to be gone, and never use `--force` unprompted.
- Every command takes `--json`, exits 0 on success and 1 on failure, and
  writes errors to stderr. `nudge help <command>` lists a command's options.
- `nudge open` opens the window; it needs their desktop, so do not wait on it.
