# Admin TUI

A terminal-based administrative dashboard for the Event Platform.
Provides real-time visibility into operational state from the same
host as the server, without requiring a web browser.

## Build

```bash
cd tui
make
```

Requires `libpq-dev` and `libncurses-dev` (`pkg install postgresql
ncurses` on Termux; both come with the postgresql package on most
Linux distributions).

## Run

```bash
./admin-tui                                                # uses default DSN
./admin-tui "postgresql://user:pass@host:5432/dbname"      # custom DSN
```

## Display

```
 Event Platform — Admin TUI                            LIVE 14:32:15

 ● Database online    PostgreSQL 18.2 (Termux)

 ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐
 │ Participants        │  │ Active sessions     │  │ Total check-ins     │
 │                  47 │  │                   3 │  │                  92 │
 └─────────────────────┘  └─────────────────────┘  └─────────────────────┘

 ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐
 │ Devices linked      │  │ Sessions (total)    │  │ Database size       │
 │                  41 │  │                  18 │  │           7.84 MiB  │
 └─────────────────────┘  └─────────────────────┘  └─────────────────────┘

 [q] quit    [r] refresh    [p] pause/resume    refresh every 3s
```

## Key Bindings

| Key | Action                                |
| --- | ------------------------------------- |
| `q` | Quit                                  |
| `r` | Refresh data immediately              |
| `p` | Pause / resume the auto-refresh loop  |

## Operational Use

The TUI is intended for situations where a graphical browser is
inconvenient or unavailable:

- Diagnosing the platform from a Termux session over SSH
- Quick visibility during an outage when the web console may be down
- Live monitoring during an event without occupying browser real estate

It is read-only by design. Mutations (creating sessions, deleting
participants, etc.) must be performed via the web console or REST API.
