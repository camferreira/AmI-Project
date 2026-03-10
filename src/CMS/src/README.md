# SMS CMS — Central Management System

Web interface for controlling and monitoring the SMS Arduino node.

## Requirements

- Python 3.11+
- [uv](https://docs.astral.sh/uv/) — install with `curl -LsSf https://astral.sh/uv/install.sh | sh`

## Install & run

```bash
# Clone / unzip the project, then:
cd cms

# Create venv + install dependencies (first run only)
uv sync

# Run the app
uv run cms
```

Then open **http://localhost:5000** in your browser.

## During development

```bash
# Add a new dependency
uv add some-package

# Run directly without installing the script
uv run python app.py
```

## Connect to Arduino

1. Plug in the Arduino via USB
2. Select the serial port from the dropdown (e.g. COM3 on Windows, /dev/ttyUSB0 on Linux, /dev/cu.usbmodem* on macOS)
3. Click Connect — the app pings the node and syncs state automatically

## Architecture

    Arduino SMS  ──USB Serial──►  app.py (Flask + SocketIO)  ──WebSocket──►  Browser UI
                     9600 baud       background reader thread     live push

- app.py                 — Flask server, serial reader thread, REST API, SocketIO
- templates/index.html   — Single-page digital twin UI
- pyproject.toml         — uv project definition and dependencies
