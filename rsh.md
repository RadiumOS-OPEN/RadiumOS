# RadiumOS Scripting Language (RSH)
## Complete Guide & Reference

---

## Table of Contents

1. [Introduction](#introduction)
2. [Getting Started](#getting-started)
3. [Window System](#window-system)
4. [UI Components](#ui-components)
5. [Variables & Logic](#variables--logic)
6. [Complete Examples](#complete-examples)
7. [Reference](#reference)

---

## Introduction

RadiumOS Scripting Language (RSH) is a powerful command-based scripting system for creating graphical applications and automating tasks in RadiumOS. With RSH, you can build interactive menus, dialog boxes, progress indicators, and complete applications using simple text-based commands.

### What Can You Build?

- **Interactive menu systems** - Navigation menus with keyboard selection
- **Installation wizards** - Multi-step processes with progress bars
- **System configuration dialogs** - Settings and preferences interfaces
- **Games with graphical interfaces** - Text-based games with menus and UI
- **File management tools** - Custom file browsers and utilities
- **Custom launchers and dashboards** - Application launchers and system monitors

### File Extension

All RadiumOS scripts use the `.rsh` extension (RadiumOS Shell Script).

---

## Getting Started

### Your First Script - Line by Line Breakdown

Let's create a file called `hello.rsh` and examine each line in detail. This simple script will create a window, display a message, and wait for user input.

**The complete script:**

Your script contains seven lines. We'll break down each one to understand exactly what happens.

---

### Line 1: Comment Line

**The line:** `% My first RadiumOS script`

**What it does:** This is a comment that documents your code. The script engine completely ignores this line.

**Details:**
- Comments start with either `%` or `#` 
- Everything after these symbols on that line is ignored
- Comments are purely for human readers
- Use them to explain what your code does
- Very useful for documenting complex logic

**When to use comments:**
- At the start of files to describe the script's purpose
- Before complex sections to explain the logic
- To mark TODO items or future improvements
- To temporarily disable code without deleting it

**Comment examples:**
- `% Initialize the game variables` 
- `# TODO: Add error handling here`
- `% Created by: John - Date: 2025-12-13`
- `# This section handles user authentication`

---

### Line 2: Creating the Window

**The line:** `win_create_centered 50 12 white blue`

**What it does:** Creates a new window automatically centered on the screen.

**Breaking down each part:**

**Command name:** `win_create_centered`
- This is the function that creates centered windows
- Alternative: `win_create` if you want to specify exact X,Y coordinates

**First number (50):** Window width
- This window will be 50 characters wide
- The screen is 80 characters wide total
- So this window uses about 62% of the screen width

**Second number (12):** Window height
- This window will be 12 rows tall
- The screen is 25 rows tall total
- So this window uses about 48% of the screen height

**First color (white):** Foreground/text color
- All text drawn in this window will be white
- This is the color of characters, borders, and text

**Second color (blue):** Background color
- The window's background fill will be blue
- This is the space behind the text

**What happens internally:**

1. The system calculates the screen center (X=40, Y=12.5)
2. It then calculates where to place the window so it's centered
3. A window object is created with the specified dimensions and colors
4. The window gets assigned a unique ID number (probably 0 for the first window)
5. **IMPORTANT:** This ID is automatically stored in the special variable `$WINID`
6. The window exists but is NOT yet visible on screen

**The magic variable $WINID:**

After running `win_create_centered`, the system automatically sets `$WINID` to contain the new window's ID number. This is incredibly convenient because you can immediately use `$WINID` in the next commands without manually storing it.

However, if you're creating multiple windows, you should save `$WINID` to your own variable right away:
- First window: `win_create_centered 50 12 white blue` then `set MAIN_WIN $WINID`
- Second window: `win_create_centered 40 10 white red` then `set DIALOG $WINID`

**Visual representation of what gets created:**

The window occupies a rectangular area in the center of your 80×25 character screen. Since it's 50 characters wide and centered horizontally, there will be 15 characters of space on the left and 15 on the right. Since it's 12 rows tall and centered vertically, there will be about 6-7 rows above and below.

**Color options available:**

Standard colors: black, blue, green, cyan, red, magenta, brown, light_grey

Light colors: dark_grey, light_blue, light_green, light_cyan, light_red, light_magenta, light_brown, white

**Common window size patterns:**
- Small alert: 30 wide × 8 tall
- Standard dialog: 50 wide × 12 tall
- Large application window: 70 wide × 20 tall
- Full screen: 80 wide × 25 tall

**Different color combinations and their uses:**
- `white blue` - Classic Windows-style dialog (professional, standard)
- `light_cyan dark_grey` - Modern dark theme (sleek, easy on eyes)
- `white red` - Error or critical alert (urgent, attention-grabbing)
- `white light_green` - Success message (positive, confirming)
- `black light_brown` - Warning dialog (cautionary, important)
- `light_green black` - Terminal/retro style (classic computing feel)

---

### Line 3: Setting the Window Title

**The line:** `win_set_title $WINID "Hello RadiumOS"`

**What it does:** Sets the text that appears in the window's title bar.

**Breaking down each part:**

**Command name:** `win_set_title`
- This function modifies a window's title bar
- Every window can have a title displayed at the top

**Window ID:** `$WINID`
- This is a variable containing the window ID
- The `$` symbol tells RSH to read the variable's value
- Since we just created a window, `$WINID` contains that window's ID
- Without the `$`, it would look for a window literally named "WINID"

**Title text:** `"Hello RadiumOS"`
- This is the text that will appear in the title bar
- Can contain spaces and most characters
- Will be displayed at the top of the window with decorative borders

**How variables work here:**

When the script engine sees `$WINID`, it:
1. Recognizes the `$` symbol means "this is a variable"
2. Looks up what value is stored in `WINID`
3. Replaces `$WINID` with that value (probably 0)
4. Executes the command with the actual window ID

So the command effectively becomes: `win_set_title 0 "Hello RadiumOS"`

**About window titles:**

Window titles appear in a special border at the top of the window. They're typically centered and surrounded by decorative characters. The title helps users identify what the window is for - like "File Manager", "Settings", "Error", etc.

**Title best practices:**
- Keep titles short (under 40 characters fits well)
- Use descriptive names that indicate the window's purpose
- For applications, include the app name: "RadiumOS File Manager"
- For dialogs, describe the action: "Confirm Delete"
- For status windows, include current state: "Installing - 50%"

**Using variables in titles:**

You can build dynamic titles using variables. For example, if you have a variable called `USERNAME` containing "Alice", you could write:

`win_set_title $WINID "Welcome, $USERNAME!"` 

This would display "Welcome, Alice!" in the title bar.

**Visual effect:**

The top border of your window will now show the title. It typically looks something like this in ASCII:

The top line becomes: `┌─[ Hello RadiumOS ]───────────────────────────┐`

The title is inserted into the border decoration, making it both functional and attractive.

---

### Line 4: Printing Centered Text

**The line:** `win_print_centered $WINID 5 Welcome!`

**What it does:** Displays the text "Welcome!" centered horizontally on row 5 of the window.

**Breaking down each part:**

**Command name:** `win_print_centered`
- This function prints text centered horizontally within the window
- Alternative: `win_print` if you want to specify both X and Y coordinates

**Window ID:** `$WINID`
- Again using the variable containing our window's ID
- This tells the system which window to draw the text in

**Row number:** `5`
- This is the Y coordinate (vertical position)
- Row 5 means 5 rows down from the top of the window's interior
- Coordinates start at 0, so row 5 is actually the 6th row
- The text will be centered on this row

**Text to display:** `Welcome!`
- This is the actual message that will appear
- It will be automatically centered based on the window's width
- Multi-word text works fine even without quotes in this case

**How centering works:**

The system performs automatic centering:
1. Takes the text "Welcome!" (8 characters including the exclamation)
2. Looks at the window's width (50 characters)
3. Calculates: (50 - 8) / 2 = 21
4. Places the text starting at position 21 from the left edge
5. This makes the text appear centered

**Why use centered text:**

Centered text is visually appealing and professional-looking. It's perfect for:
- Titles and headers within windows
- Important messages that should draw attention
- Instructions and prompts
- Status messages
- Menu headings

**Row positioning explained:**

Remember that row 0 is the very top interior line of the window (just below the title bar). So:
- Row 0: Top line (just under title)
- Row 1: Second line
- Row 5: Sixth line down (roughly in the upper-middle area)
- For a 12-tall window, row 6 would be the exact middle

**Coordinate system:**

Windows in RSH use a coordinate system where:
- X coordinates go left to right (0 is leftmost)
- Y coordinates go top to bottom (0 is topmost)
- Coordinates are relative to the window's interior (not the screen)
- The border doesn't count in the coordinate system

**Alternative positioning:**

If you wanted the text at a specific position instead of centered, you'd use `win_print`:

`win_print $WINID 10 5 Welcome!` would put "Welcome!" at position X=10, Y=5 (10 characters from left, 5 rows down).

---

### Line 5: Refreshing the Window

**The line:** `win_refresh $WINID`

**What it does:** Updates the visual display of the window to show all the changes we've made.

**Why this is critical:**

This is one of the most important concepts in RSH programming. When you draw text, create buttons, or make any visual changes to a window, those changes are made to the window's internal memory buffer but are NOT immediately shown on screen. You must call `win_refresh` to actually update the display.

**Think of it like a canvas:**
- The window is like a canvas
- Commands like `win_print` are like painting on the canvas
- But the canvas is face-down on the table
- `win_refresh` is like flipping the canvas over so everyone can see your artwork

**Performance reason:**

This two-step process (make changes, then refresh) exists for performance. If the window updated after every single command, it would flicker and be slow. Instead, you can make many changes in memory, then refresh once to show them all at the same time.

**When to call win_refresh:**

Call it after you've finished a "batch" of updates:
- After drawing all your text
- After creating all your UI elements in a window
- After clearing and redrawing window content
- Before showing the window to the user
- After any visual change you want the user to see

**What happens without it:**

If you forget `win_refresh`, your window will appear empty or show outdated content even though you've written text to it. The changes exist in memory but haven't been rendered to the screen.

**Common pattern:**

Most scripts follow this pattern:
1. Create window
2. Set title
3. Draw all text and UI elements
4. Call `win_refresh` once
5. Show the window with `win_show`

**Multiple refreshes:**

You can call `win_refresh` as many times as needed. For example, in an animation or progress bar, you might refresh after each update:
- Update progress to 25%
- Refresh window
- Wait 500ms
- Update progress to 50%
- Refresh window
- And so on...

---

### Line 6: Waiting for User Input

**The line:** `wait_key`

**What it does:** Pauses the script execution until the user presses any key on the keyboard.

**Why this is important:**

Without this line, the script would create the window, display the text, and immediately continue to the next line (which hides the window). The window would flash on screen for a split second and disappear before the user could read it.

`wait_key` creates an interactive pause point where the user controls when the script continues.

**How it works:**

1. Script execution stops at this line
2. The system waits for keyboard input
3. User presses any key (letter, number, space, enter, escape, etc.)
4. The key press is captured
5. Script execution continues to the next line

**User experience:**

From the user's perspective:
- The window appears with the message "Welcome!"
- The window stays visible and waits
- User can read the content at their own pace
- When ready to continue, user presses any key
- The window disappears and the script ends

**Alternative command:**

The command `pause` does exactly the same thing. Both are interchangeable:
- `wait_key` - waits for keyboard input
- `pause` - waits for keyboard input (same behavior)

Use whichever you find more readable.

**Common usage patterns:**

**Pattern 1: Simple "press any key to continue"**
After displaying information, wait for acknowledgment before continuing.

**Pattern 2: End of script**
Keep a window visible until the user is ready to close it.

**Pattern 3: Step-through tutorials**
Show one screen of information, wait for key press, show next screen.

**Pattern 4: Splash screens**
Display a logo or welcome message, wait briefly, then continue.

**Better user experience:**

It's good practice to tell the user what to do. Instead of just `wait_key`, first print an instruction:

Add a line before it: `win_print_centered $WINID 10 "Press any key to continue..."`

Then call `wait_key`. Now the user knows what's expected.

**What keys are detected:**

The `wait_key` command accepts ANY keyboard input:
- Letter keys (a-z, A-Z)
- Number keys (0-9)
- Special keys (Enter, Space, Escape)
- Function keys
- Arrow keys
- Basically any key press will trigger it

The command doesn't care which specific key was pressed - it just waits for any input.

---

### Line 7: Hiding the Window

**The line:** `win_hide $WINID`

**What it does:** Removes the window from the screen and frees up that window slot for reuse.

**Breaking down the behavior:**

**Command name:** `win_hide`
- This function hides a window and marks its slot as available

**Window ID:** `$WINID`
- The window we want to hide (the one we created earlier)

**Why hide windows:**

There are two important reasons to hide windows when you're done with them:

**Reason 1: Visual cleanup**
The window disappears from the screen, returning the display to its previous state. If you don't hide the window, it stays visible even though the script is finished.

**Reason 2: Resource management**
RadiumOS has a limit of 8 windows maximum at any time. When you hide a window, you free up that slot so it can be reused for a new window. If you create 8 windows and never hide any, the 9th `win_create` command will fail with an error.

**What happens to the window:**

When you call `win_hide`:
1. The window is erased from the screen
2. The window slot is marked as "not used"
3. The window ID number becomes available for reuse
4. Any variables storing that window ID (like `$WINID`) still contain the number, but it's no longer valid

**After hiding:**

Once hidden, you cannot use that window ID anymore. If you try to call `win_print $WINID ...` after hiding it, nothing will happen (or you'll get an error) because that window no longer exists.

**Window lifecycle:**

A typical window goes through this lifecycle:
1. **Create**: `win_create_centered` - window exists in memory but invisible
2. **Setup**: Set title, draw text, add UI elements
3. **Show**: `win_show` - window becomes visible
4. **Interact**: User interacts with it, reads content, makes selections
5. **Hide**: `win_hide` - window disappears and resources are freed

**Cleanup best practice:**

Always hide windows when you're done with them. It's like cleaning up after yourself - good programming hygiene. Even if your script is ending, explicitly hiding windows makes your intent clear and ensures resources are properly released.

**Multi-window management:**

If you have multiple windows, hide them in reverse order of creation (like a stack). This prevents visual glitches where older windows briefly reappear.

---

### Line 8: Showing a Toast Notification

**The line:** `toast "Done!"`

**What it does:** Displays a brief, small notification message that automatically disappears after a short time.

**Breaking down the command:**

**Command name:** `toast`
- This function shows minimal, temporary notifications
- Named after "toast" notifications in modern operating systems
- Think of Android/phone notifications that "pop up" briefly

**Message text:** `"Done!"`
- The text to display in the notification
- Quotes are recommended for multi-word messages
- Keep it short - toasts are meant for brief messages

**Toast characteristics:**

**Automatic timing:** Toasts appear for about 2-3 seconds then automatically disappear. You don't need to manually hide them or wait for user input.

**Minimal design:** Toasts are small, unobtrusive notifications. They don't block the entire screen like a dialog box would.

**No user interaction required:** Unlike windows that need `wait_key`, toasts are fire-and-forget. The script continues immediately.

**Use cases for toasts:**

**Quick confirmations:** "Settings saved", "File deleted", "Copy successful"

**Status updates:** "Connected", "Disconnected", "Loading complete"

**Brief information:** "3 items selected", "Task complete", "Operation cancelled"

**Non-critical messages:** Things the user should know but don't require acknowledgment

**Toast vs. Window:**

**Use a toast when:**
- The message is brief (1-5 words)
- User doesn't need to acknowledge it
- It's informational, not critical
- You want script execution to continue immediately

**Use a window when:**
- The message is longer or complex
- User needs to read and acknowledge
- You need user input or decision
- The information is important/critical

**Toast vs. Notify:**

RSH has three notification systems:
- `toast` - Minimal, brief, auto-disappears
- `notify` - Standard notification with type (info/success/warning/error) and duration control
- `notify_titled` - Notification with a title and message

For this simple "Done!" message, `toast` is perfect - it's quick, unobtrusive, and doesn't require any additional parameters.

---

### Putting It All Together

Now let's see how all seven lines work together as a complete program:

**Execution flow:**

1. Script starts
2. Comment line is skipped
3. Window is created (centered, 50×12, white on blue) - ID stored in `$WINID`
4. Window title is set to "Hello RadiumOS"
5. Text "Welcome!" is drawn centered on row 5
6. Window display is refreshed to show the changes
7. Window becomes visible (wait, we're missing `win_show`!)
8. Script pauses, waiting for keyboard input
9. User presses a key
10. Window is hidden and resources freed
11. Toast "Done!" appears briefly
12. Script ends

**Wait - there's a missing step!**

Actually, this script is missing one important command: `win_show $WINID`. The window is created and prepared but never explicitly shown. Depending on your RSH implementation, the window might:
- Be visible by default after creation
- Require explicit `win_show` to display
- Only refresh makes it visible

**Better version with explicit show:**

Insert `win_show $WINID` after `win_refresh $WINID` to be explicit about when the window appears.

**What the user experiences:**

1. Screen is empty (normal terminal/desktop)
2. Window appears in center of screen with blue background
3. Title bar shows "Hello RadiumOS"
4. Center of window shows "Welcome!"
5. User reads the message
6. User presses any key
7. Window disappears
8. Small toast notification appears showing "Done!"
9. Toast fades away after 2-3 seconds
10. Back to normal screen

**Total lines of code: 7 (excluding comment)**

With just 7 simple commands, you've created a complete graphical application with:
- A window
- A title
- Centered text
- User interaction
- Cleanup
- Confirmation feedback

This demonstrates the power and simplicity of RSH!

---

### Running Your Script

**To execute the script:**

Save the file as `hello.rsh` in your RadiumOS filesystem, then run it with the command:

`run hello.rsh`

Or if you're in the directory containing the script:

`run hello`

The `.rsh` extension may be optional depending on your system configuration.

**Troubleshooting:**

**Window doesn't appear:** Add `win_show $WINID` after `win_refresh`

**Text doesn't show:** Make sure `win_refresh` is called after drawing text

**Script exits too quickly:** Confirm `wait_key` is present and the window is shown

**"No free window slots" error:** You're creating too many windows - make sure to `win_hide` when done

**Variables not working:** Check that you're using `$` before variable names

---

### Next Steps

Now that you understand the basics, try modifying the script:

**Experiment 1:** Change the window size to 60×15

**Experiment 2:** Change colors to light_green and black for a terminal look

**Experiment 3:** Add more text lines at different Y positions

**Experiment 4:** Change the title to something else

**Experiment 5:** Add a second message after the user presses a key

**Experiment 6:** Create multiple windows in sequence

These experiments will help you understand how each parameter affects the output and build your confidence with RSH programming.

[Back to top](#complete-guide--reference)

---

## Window System

### Understanding Window Creation

Windows are the foundation of all graphical RSH applications. Every UI element - text, buttons, menus, progress bars - must be drawn inside a window. Think of windows as containers for your application's interface.

### Window Coordinate System

**Screen dimensions:** RadiumOS uses an 80-column by 25-row text display

**Coordinate origin:** Position (0, 0) is the top-left corner of the screen

**X-axis:** Runs left to right, values 0-79

**Y-axis:** Runs top to bottom, values 0-24

**Window interior coordinates:** When drawing inside a window, coordinates are relative to the window's top-left interior corner (not including the border)

### The Two Window Creation Commands

RSH provides two ways to create windows, each suited for different purposes.

#### win_create - Manual Positioning

**Purpose:** Create a window at an exact screen position you specify

**Syntax:** `win_create <x> <y> <width> <height> [fg_color] [bg_color]`

**When to use manual positioning:**

**Tiled layouts:** When you want multiple windows side-by-side or in specific arrangements

**Fixed positions:** Status bars at the bottom, toolbars at the top, sidebars on edges

**Pixel-perfect placement:** When exact positioning matters for your design

**Non-centered designs:** Asymmetric layouts, corner notifications

**Examples of manual positioning:**

`win_create 0 0 40 25 white blue` - Left half of screen, full height

`win_create 40 0 40 25 white blue` - Right half of screen, full height

`win_create 0 20 80 5 black light_grey` - Bottom status bar

`win_create 60 2 18 6 white red` - Top-right corner notification

`win_create 10 5 30 15 light_cyan dark_grey` - Specific offset position

**Calculating positions:**

To center a window manually: X = (80 - width) / 2, Y = (25 - height) / 2

To align right: X = 80 - width

To align bottom: Y = 25 - height

#### win_create_centered - Automatic Centering

**Purpose:** Create a window automatically centered on the screen

**Syntax:** `win_create_centered <width> <height> [fg_color] [bg_color]`

**When to use centered windows:**

**Dialog boxes:** Confirmation dialogs, alerts, prompts

**Main application windows:** Primary interface for apps

**Splash screens:** Welcome messages, loading screens

**Focus attention:** Important messages that should be immediately visible

**Simplicity:** When you don't care about exact position, just want it centered

**Examples of centered windows:**

`win_create_centered 50 15` - Standard application window (uses default colors)

`win_create_centered 40 10 white red` - Error dialog

`win_create_centered 60 18 light_cyan dark_grey` - Large main window

`win_create_centered 30 8 white light_green` - Success message

**Advantages of centering:**
- No coordinate calculation needed
- Automatically adapts if you change window size
- Looks professional and polished
- User's eyes naturally go to the center

### Color System Deep Dive

RSH supports 16 VGA colors divided into standard and bright variations.

**Standard colors (darker):**

`black` - True black, color value 0

`blue` - Dark blue, traditional Windows dialog color

`green` - Dark green, forest/terminal green

`cyan` - Dark cyan, teal-like color

`red` - Dark red, burgundy tone

`magenta` - Dark magenta/purple

`brown` - Brown/dark yellow/orange

`light_grey` - Light gray, good for backgrounds

**Bright colors (lighter):**

`dark_grey` - Dark gray, brighter than black

`light_blue` - Bright blue, vibrant

`light_green` - Bright green, vivid

`light_cyan` - Bright cyan, aqua

`light_red` - Bright red, attention-grabbing

`light_magenta` - Bright magenta/pink

`light_brown` - Yellow, actually looks yellow not brown

`white` - Brightest white, maximum contrast

**Color combination psychology:**

`white on blue` - Professional, trustworthy, Windows-like

`light_cyan on dark_grey` - Modern, sleek, dark mode

`white on red` - Urgent, error, stop

`black on light_brown` - Warning, caution

`white on light_green` - Success, go, positive

`light_green on black` - Retro terminal, hacker aesthetic

`black on light_grey` - Neutral, disabled state

**Readability tips:**
- High contrast is best: light foreground on dark background or vice versa
- Avoid similar colors: red on magenta, blue on cyan
- Consider colorblind users: don't rely solely on color to convey information

### The Magic $WINID Variable

When you create a window, RSH automatically stores its ID in the special variable `$WINID`. This is one of the most convenient features of the language.

**How it works:**

Every window creation command automatically performs: `set WINID <new_window_id>`

You don't have to do this manually - it happens behind the scenes

The value in `$WINID` is the window's unique identifier number (0-7)

**Why this matters:**

You can immediately reference the window in the next command using `$WINID`

No need to manually track window IDs for simple scripts

Clean, readable code without extra variable assignments

**When to save $WINID:**

If you're creating multiple windows, save each ID immediately:

`win_create_centered 50 15 white blue`
`set MAIN_WINDOW $WINID`
`win_create_centered 40 10 white red`
`set ERROR_DIALOG $WINID`

Now you have two separate variables and can control each window independently.

**Window ID numbers:**

IDs range from 0 to 7 (8 windows maximum)

First window created gets ID 0, second gets ID 1, etc.

When you hide a window, its ID becomes available for reuse

**Common mistake:**

Creating multiple windows and only using `$WINID` - it will only reference the most recently created window! Always save to your own variables for multi-window applications.

### Window Operations in Detail

#### win_show - Making Windows Visible

**Purpose:** Display a window on screen

**Syntax:** `win_show <window_id>`

**Detailed behavior:**

Windows are created in a hidden state by default (depends on implementation)

`win_show` makes the window visible to the user

The window appears with all content you've drawn to it

Multiple windows can be visible simultaneously (up to 8)

Windows appear in layers - more recent windows may cover older ones

**When to call win_show:**

After creating and populating a window with all content

After calling `win_refresh` to ensure content is ready

Before any user interaction with the window

**Pattern:**
Create → Add content → Refresh → Show → Interact

#### win_hide - Removing Windows

**Purpose:** Remove window from screen and free its slot

**Syntax:** `win_hide <window_id>`

**Critical importance:**

Maximum 8 windows can exist at once

`win_hide` frees a window slot for reuse

Always hide windows when finished to prevent resource exhaustion

**What happens when you hide:**

Window disappears from screen immediately

Window slot marked as available

Window ID can be reused for new windows

Any data in the window is lost

**Best practices:**

Hide windows in reverse order of creation (newest first)

Hide temporary dialogs before showing new windows

Always hide at end of script to clean up resources

Create reusable window "pools" by hide/show instead of create/destroy

#### win_clear - Erasing Window Contents

**Purpose:** Remove all text and graphics from window interior

**Syntax:** `win_clear <window_id>`

**What it clears:**

All text drawn with `win_print` or `win_print_centered`

All UI elements like buttons, menus, progress bars

Fills entire window interior with the background color

**What it doesn't clear:**

Window title (title bar remains unchanged)

Window borders

Window position and size

Window colors

**Use cases:**

Redrawing window with new content

Clearing output before showing new information

Resetting window state

Creating multi-screen wizards or tutorials

**Pattern for redrawing:**

Clear → Draw new content → Refresh

Example: `win_clear $WIN` then `win_print $WIN 5 5 New message` then `win_refresh $WIN`

#### win_refresh - Updating the Display

**Purpose:** Render all pending changes to the screen

**Syntax:** `win_refresh <window_id>`

**Critical concept - double buffering:**

When you draw text or UI elements, changes go to a memory buffer

The screen doesn't update until you call `win_refresh`

This prevents flickering and improves performance

Think of it as "apply changes" or "update display"

**When to refresh:**

After drawing text with `win_print` or `win_print_centered`

After drawing UI elements like buttons or progress bars

After calling `win_clear`

Before showing a window with `win_show`

After any batch of visual changes

**How often to refresh:**

For static content: Once after all drawing is complete

For animations: After each frame update

For progress bars: After each progress increment

For interactive elements: After each state change

**Performance note:**

Refreshing is relatively expensive - avoid excessive calls

Batch your changes then refresh once rather than refresh after every draw call

For 60 FPS animation, refresh maximum 60 times per second

#### win_set_title - Customizing Title Bars

**Purpose:** Set or change the window's title bar text

**Syntax:** `win_set_title <window_id> <title_text>`

**Title display:**

Appears in the top border of the window

Typically centered with decorative characters

Helps users identify the window's purpose

**Title guidelines:**

Keep under 40 characters for best appearance

Use descriptive names that indicate function

Include version numbers or status in title

Can be changed dynamically to show progress or state

**Dynamic titles:**

Update title to show changing information

Progress: `win_set_title $WIN "Installing - 45%"`

Status: `win_set_title $WIN "File Manager - 12 items"`

Time: `win_set_title $WIN "Clock - 14:35:27"`

State: `win_set_title $WIN "Game - Level 3"`

**Empty title:**

`win_set_title $WIN ""` removes the title text while keeping the border

#### win_move - Repositioning Windows

**Purpose:** Change window position without recreating it

**Syntax:** `win_move <window_id> <new_x> <new_y>`

**Use cases:**

Animating window movement across screen

Rearranging windows dynamically

Sliding windows in from edges

Creating window "snap" to screen edges

**Animation example concept:**

Start window off-screen at X=-50

Loop: Move right by 1 pixel, refresh, delay 10ms

End at centered position

Creates sliding animation effect

**Performance consideration:**

Moving windows is relatively fast

Combine with delays for smooth animation

Too fast movement may appear jerky

Too slow movement may seem sluggish

**Boundary checking:**

Ensure window stays within screen bounds (0-79, 0-24)

Don't move window partially off-screen

Check: new_x >= 0, new_x + width <= 80

Check: new_y >= 0, new_y + height <= 25

---

[Continuing in next section due to length - would you like me to continue with the UI Components section and beyond?]

[Back to top](#complete-guide--reference)