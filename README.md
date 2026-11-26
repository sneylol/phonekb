# Phone Keyboard for Flipper Zero

Use your Android phone as a **wireless keyboard and touchpad** for any computer, with your Flipper Zero as the receiver.

```
 Phone (Chrome)  ──Bluetooth──▶  Flipper Zero  ──USB cable──▶  PC
                                                   sees a normal USB keyboard + mouse
```

The computer needs **no drivers, no software and no Bluetooth**. It just sees a plain USB keyboard and mouse, so it also works in BIOS/UEFI setup, on locked-down work PCs, Raspberry Pis, smart TVs, and anything else that accepts a USB keyboard.

**Open the phone controller:** https://carlantaya.github.io/phonekb/

![Main screen, extra keys, and the macro editor](docs/screenshots.png)

## Features

- **Touchpad:** drag to move, tap to click, two-finger tap to right-click, two-finger drag to scroll, double-tap and hold to drag
- **Hold click:** tap once to hold the left button down (for dragging windows or selecting text), tap again to release
- **Adjustable touch speed**, remembered on your phone
- **Type with your normal phone keyboard**, including autocorrect and swipe typing (corrections are fixed on the PC with backspaces)
- **Special keys:** Ctrl / Alt / Win / Shift (sticky), Esc, Tab, Caps Lock, arrows, Home/End, PgUp/PgDn, F1–F12, Print Screen, Ctrl+Alt+Del
- **Shortcuts:** Copy, Paste, Undo, Select all, Alt+Tab, Show desktop
- **Media keys:** volume, mute, play/pause, next track
- **Macros:** record what you do, edit the steps (keys, text, clicks, mouse moves, waits), and replay them, as many times as you like
- **Import / export:** save all your macros and groups, or just the ones you pick, to a file and load them on another phone
- **Remote:** sort macros into groups (e.g. Work, Gaming) and get a page of big buttons, one tap per macro
- **Send a block of text:** paste a paragraph on the phone and have it typed out on the PC. **Clipboard** types what you last copied on the phone. With the mntm-012 build the whole text is sent to the Flipper first (up to 20,000 characters) and the Flipper types it by itself, so locking the phone or losing the connection doesn't stop it. Pause / Resume / Stop work from the phone, Back stops it on the Flipper. While the Flipper types it, the touchpad and keys on the phone are off (they'd only act once the text is done).
- **Write with Claude:** tap **Write**, take photos of a task with the camera or pick them from the gallery (several at once is fine), type the task into the box, or both (e.g. photos plus "only do task 2"), then tap **Write text**. The photos go to Claude (Claude Opus 5.5) together with your saved reference images and instruction; the answer lands in the text box and is stored on the Flipper, so a Caps Lock press on the PC types it (Caps Lock again pauses / resumes). You can edit the text before typing it, and pasting text by hand works as before.
  - Needs your own Anthropic API key (console.anthropic.com, pay per use: with about 20 reference images roughly 40 cents for the first task and about 10 cents for each further one within 5 minutes (the references are cached); the page shows the cost after each answer). Enter it under **Claude settings**; it stays on the phone, like the references (**Add references**: images, PDFs, Word, PowerPoint, Excel, OpenDocument, EPUB, text, Markdown, HTML or RTF files; old .doc/.ppt/.xls and Apple Pages/Keynote files need saving as PDF or .docx first). PDFs are sent whole, so Claude also sees their pictures; from the other documents it gets the text. Keep the page open until the text is there (about 20–60 s), then the phone can be locked.
  - If Claude Opus declines a task, the API retries it on a fallback model automatically.
  - Claude works only with the text given in the task: passages left out with (…) are not used, even if Claude knows the full original or a reference contains it. References are models for structure and style. If the task is only a short typed text (no photos) that names a text, e.g. a speech, without giving it, Claude can search the web for it (with photos or a pasted text it can't go online at all); that costs a bit more and the found version may differ from your sheet, so giving the text is more exact.
  - The answer is typed exactly as it is: no Markdown and no word count line (the page shows the word count on the phone instead).
- **Humanize (mntm-012 build):** Send text, Clipboard and macro text are typed like a person: natural rhythm, keyboard geometry, pauses after punctuation, realistic key hold times. Choose the speed (15–150 WPM), optional **typos** (a neighbouring key, fixed with Backspace) and **thinking pauses** (Off / Light / Normal / Heavy: pauses every few sentences and before new paragraphs). Live typing on the phone keyboard is never delayed.
- **Letter delay:** a pause after each typed letter (0 ms – 5 s), for apps or PCs that drop fast input.
- **Works offline:** install it from Chrome and it runs with no internet
- **Caps Lock on the PC (mntm-012 build, Windows and Linux):**
  - **While the Flipper types (or a macro plays):** Caps Lock pauses, Caps Lock again resumes (or tap Resume on the phone).
  - **While it's idle ("Caps Lock types clipboard", on by default):** text pasted into the Send text box is stored on the Flipper ("✓ Stored on the Flipper"); a Caps Lock press then makes the Flipper type it by itself, even with the phone locked. It's used once; edit or paste new text to store it again. Texts over 20,000 characters aren't stored (use Send text). With an empty box, the phone's clipboard is typed instead (Bluefy open and the screen on), falling back to the box. Turn the switch off if you use Caps Lock normally and don't want this.
  - Text is always typed with Caps Lock off (with it on, some PCs shift every key: `,` → `;`, `?` → `ß`); Caps Lock is put back the way it was afterwards.
  - Plugging the Flipper into a PC that has Caps Lock on doesn't start anything: that first state is ignored.
  - **On a Mac** this doesn't work: macOS doesn't pass a Caps Lock press on the Mac's own keyboard on to other keyboards, so the Flipper never sees it. Use Pause on the phone. Also keep Caps Lock off on a Mac while the Flipper types, otherwise letters come out with the wrong case.
  - (The app brings its own USB keyboard for this: Momentum's can't receive the Caps Lock state. To the PC it's the same device.)
- **No pairing codes (Momentum mntm-012 build):** the page keeps a random secret key and the Flipper only accepts the first phone that connects, so other nearby devices can't type on your PC. The link isn't Bluetooth-encrypted, so someone sniffing Bluetooth nearby could read what you type. The other `.fap` builds still use a 6-digit PIN.

## Requirements

- Flipper Zero running **Official, Momentum, Unleashed or RogueMaster** firmware (see the table below)
- Android phone with **Chrome** (iPhone isn't supported: iOS browsers don't have Web Bluetooth)
- A USB-C cable from the Flipper to the computer

## Setup

### 1. Install the Flipper app

Check your firmware and version on the Flipper under **Settings → System Info** (or *About*), then download the matching file:

| Firmware | Version | Download |
|---|---|---|
| **Official** (Flipper Devices) | 1.5.1 | [phone_kb_ofw-1.5.1.fap](https://github.com/CarlAntaya/phonekb/raw/main/release/phone_kb_ofw-1.5.1.fap) |
| **Momentum** | mntm-012 | [phone_kb_mntm-012.fap](https://github.com/CarlAntaya/phonekb/raw/main/release/phone_kb_mntm-012.fap) |
| Momentum | mntm-010, mntm-011 | [phone_kb_mntm-010_011.fap](https://github.com/CarlAntaya/phonekb/raw/main/release/phone_kb_mntm-010_011.fap) |
| Momentum | mntm-007 | [phone_kb_mntm-007.fap](https://github.com/CarlAntaya/phonekb/raw/main/release/phone_kb_mntm-007.fap) |
| **Unleashed** | unlshd-095 | [phone_kb_unlshd-095.fap](https://github.com/CarlAntaya/phonekb/raw/main/release/phone_kb_unlshd-095.fap) |
| **RogueMaster** | builds from Nov 2026 | [phone_kb_roguemaster-2026-11.fap](https://github.com/CarlAntaya/phonekb/raw/main/release/phone_kb_roguemaster-2026-11.fap) |

Running something else, or an older or newer version? Try the file for the same firmware family first. If the Flipper says "App Too New" or "App Too Old", see *Building from source* below, or open an issue.

Copy the file to the Flipper's SD card in **`apps/USB/`**. Any of these ways works:
- **qFlipper** on a PC (File Manager → SD Card → apps → USB)
- **[lab.flipper.net](https://lab.flipper.net)** in Chrome on a PC
- the **Flipper phone app's** File Manager, if your version has one
- putting the SD card in a card reader

> If the Flipper says **"App Too New"** or **"App Too Old"**, press **Cancel**, not Continue. That file doesn't match your firmware and running it will crash the Flipper.

### 2. Open the phone controller

Open **https://carlantaya.github.io/phonekb/** in **Chrome on Android**, then use Chrome's **⋮ menu → Install app** (or *Add to Home screen*). After the first visit it works with no internet.

### 3. Connect

1. Plug the Flipper into the computer with the USB cable.
2. Close the Flipper phone app and qFlipper, so nothing else is connected to the Flipper.
3. On the Flipper, open **Apps → USB → Phone Keyboard**.
4. On the phone, make sure **Bluetooth and Location** are on, tap **Connect**, and choose **PhoneKB …**.
5. With the mntm-012 build there's no PIN: the first phone that connects is linked automatically. With the other builds, the first time the phone asks for a PIN: type the **6 digits shown on the Flipper**.

On the Flipper: **Back** exits, **hold Left** forgets the linked phone (the next phone that connects becomes the linked one).

## Troubleshooting

| Problem | Fix |
|---|---|
| "Web Bluetooth isn't available" | Use Chrome on Android, and open the https:// link (not a downloaded file). |
| PhoneKB doesn't appear in the list | Make sure the app is open on the Flipper, phone Location is on, and the Flipper phone app is closed. |
| Typing stops when the phone locks | Use Send text / Clipboard / the stored text (mntm-012 build): the text is on the Flipper and keeps going. Only very long texts (over 20,000 characters) and macros still need the phone awake. |
| Letters come out in the wrong case on a Mac | Caps Lock is on: macOS doesn't tell the Flipper. Switch it off. |
| "Wrong key" | The Flipper is linked to another phone or browser (or this browser's data was cleared). Hold Left on the Flipper, then tap Connect. |
| Pairing fails (PIN builds) | Hold Left on the Flipper to forget old pairings, remove "PhoneKB" from Android's Bluetooth settings, and try again. |
| Flipper crashes with "HardFault" | Wrong `.fap` for your firmware version; see the table above. |
| Wrong symbols typed (Y/Z swapped, missing umlauts) | The PC's keyboard layout must match the page: `GERMAN_PC` in `index.html` (`true` = German, `false` = US). The mntm-012 build types German layout including ä ö ü ß € @; other builds are US only. Emoji are skipped. |

## Limitations

- Android + Chrome, or iPhone with a Web Bluetooth browser such as Bluefy. (Bluefy can't reconnect by itself after a page reload: tap Connect and pick the Flipper.)
- US or German PC keyboard layout (German needs the mntm-012 build).
- While the app is running, qFlipper and the Flipper phone app can't connect to the Flipper.

## Building from source

The Flipper app is in [`flipper_app/`](flipper_app). With [ufbt](https://github.com/flipperdevices/flipperzero-ufbt):

```sh
pip install ufbt
ufbt update                   # Official firmware SDK
# or, for Momentum:
# ufbt update --index-url=https://up.momentum-fw.dev/firmware/directory.json
cd flipper_app && ufbt        # output: dist/phone_kb.fap
```

For mntm-007 and older, add `cdefines=["PHONE_KB_LEGACY_GAP"]` to `application.fam`.

The phone controller is just the static files in the repo root (`index.html`, `sw.js`, `manifest.json`, icons), so you can host them anywhere with https. When you change them, bump `VERSION` in `sw.js` so installed copies update.

### How it works

The Flipper app swaps the Flipper's Bluetooth profile for its own, which uses the Flipper serial GATT service under a separate name, MAC address and bond store, so it doesn't interfere with the official app's pairing. It switches the USB port into HID keyboard + mouse mode. The phone page talks to it with [Web Bluetooth](https://developer.mozilla.org/docs/Web/API/Web_Bluetooth_API) and sends small binary packets, which the Flipper turns into USB HID reports.

**Protocol** (phone → Flipper, written to characteristic `19ed82ae-ed21-4c9d-4145-228e62fe0000` in service `8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000`; several packets may be sent in one write):

| Bytes | Meaning |
|---|---|
| `01 mods key` | tap a key (HID usage ID + modifier bitmask) |
| `02 dx dy` | mouse move (signed bytes) |
| `03 btn` / `04 btn` | mouse button down / up (1 left, 2 right, 4 middle) |
| `05 d` | scroll wheel (signed byte) |
| `06` | release everything |
| `07 lo hi` | consumer key tap (volume, play/pause…) |
| `08 len text…` | type ASCII text |
| `09 mods key` / `0A mods key` | key down / key up |

## License

MIT, see [LICENSE](LICENSE).
