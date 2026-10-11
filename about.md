# Click Indicator

A handcam replacement: shows an image while you're **not** clicking and a different one while you **are** clicking or holding. It's drawn inside the game itself, so it works in **fullscreen** and shows up in **OBS / game capture** with no extra overlay.

No new menus, buttons or screens. Everything is in the normal mod settings, and changes apply instantly (even from the pause menu).

## Works with macros and bots

It follows the game's own button input, so replays from bots such as XDBot or Eclipse make the indicator click too.

## Features

**Images**
- Use **your own images** (PNG recommended; transparent edges are cropped automatically so the picture sits right where you put it)
- Five built-in skins: Mouse, Spacebar, Gamepad, Dot, Arrow key
- Optional **long-hold image** that appears after you've held for a set time
- Separate **Player 2 indicator** with its own images and position

**Timing**
- **Minimum click time**: keeps the clicking image up long enough to be seen, even on the fastest taps
- **Press delay** and **release delay**: line the indicator up with a video that lags behind the game
- **Fade time** for smooth swaps, and an **idle timeout** that fades the idle image out when you stop clicking

**Position and look**
- 9 screen positions, size, margins (negative values push it off-screen), rotation, extra tilt while clicking, flip horizontal/vertical
- Separate opacity and colour tint for idle and clicking, optional coloured backdrop

**Effects**
- Click **pop** (the image swells a little when you click)
- Click **ripple** (a ring expands and fades out), with colour, size and duration

**Smart behaviour**
- Left/right (A/D) **never** count in classic levels. In platformer levels they count only if you turn that on.
- Choose classic levels only, platformer levels only, or all levels
- Track both players, only Player 1, or only Player 2
- Clears itself on death, restart and pause so it never gets stuck on "clicking"
- Re-pins itself if you resize the window or toggle fullscreen

## Getting started

Install it and play a level. A built-in mouse icon appears in the bottom-right corner. To use your own pictures, open the mod's settings and pick an **Idle image** and a **Clicking image**.
