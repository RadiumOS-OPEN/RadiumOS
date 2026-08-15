```
================================================================
RADIUMOS KEYBOARD REFERENCE
================================================================

FREEZE / SCREEN MODE
----------------------------------------------------------------
F7                    Freeze screen - copy mode (normal highlight)
F8                    Freeze screen - blur mode (live pixelation)
Escape                Exit freeze, restore screen
Shift+Arrow           Extend selection in freeze mode
Home / End            Jump to start/end of row in freeze mode
Ctrl+Shift+C          Copy selection to host clipboard via serial
F6                    Toggle pixelation (no selection = whole screen)

NAVIGATION
----------------------------------------------------------------
Left / Right          Move cursor left / right
Up / Down             Command history prev / next
Home                  Jump to start of line
End                   Jump to end of line
Ctrl+Left             Jump word left
Ctrl+Right            Jump word right
Shift+Left            Extend selection left
Shift+Right           Extend selection right
Shift+Home            Extend selection to start of line
Shift+End             Extend selection to end of line
Ctrl+Shift+Left       Extend selection by word left
Ctrl+Shift+Right      Extend selection by word right
Page Up               Scroll history up one full page
Page Down             Scroll history down one full page
Ctrl+Shift+Up         Scroll history up 5 lines
Ctrl+Shift+Down       Scroll history down 5 lines
Ctrl+Home             Jump to top of history
Ctrl+End              Jump to bottom of history (live view)
Ctrl+P                Previous command (alternate Up)
Ctrl+N                Next command (alternate Down)

EDITING
----------------------------------------------------------------
Backspace             Delete character before cursor
Delete                Delete character after cursor
Ctrl+Backspace        Delete word backward
Ctrl+W                Delete word backward
Ctrl+K                Kill to end of line (cut to clipboard)
Ctrl+U                Kill to start of line (cut to clipboard)
Ctrl+T                Transpose two characters before cursor
Ctrl+Z                Undo last edit
Tab                   Autocomplete command

COPY / PASTE / CUT
----------------------------------------------------------------
Ctrl+Shift+C          Copy selection to clipboard
Ctrl+Shift+V          Paste from clipboard
Ctrl+Shift+X          Cut selection to clipboard
Ctrl+Y                Paste from clipboard (alternate)

CURSOR / SELECTION
----------------------------------------------------------------
Ctrl+A                Jump to start of line
Ctrl+Shift+A          Select all text on line
Ctrl+E                Jump to end of line

ALT / META
----------------------------------------------------------------
Alt+B                 Jump word backward
Alt+F                 Jump word forward

SPECIAL
----------------------------------------------------------------
Caps Lock             Toggle caps lock (updates LED)
F3                    Show system status info
F4                    Trigger system reboot
Ctrl+C                Cancel current input (SIGINT)
Ctrl+L                Clear screen

================================================================
FREEZE MODE WORKFLOW
================================================================

COPY MODE (F7):
  1. Press F7          - screen freezes
  2. Shift+Arrows      - highlight region (cyan)
  3. Ctrl+Shift+C      - copy to host clipboard via serial
  4. Escape            - unfreeze

BLUR MODE (F8):
  1. Press F8          - screen freezes
  2. Shift+Arrows      - selection is pixelated live as you drag
  3. F6                - toggle blur on/off
  4. Escape            - unfreeze and restore screen

================================================================
```