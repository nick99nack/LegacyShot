LegacyShot for Windows
======================

A screenshot tool for Windows 95 and up.
Plain Win32 C (no MFC, no third-party libraries), built with Visual C++ 6.0.
Designed to be used with LegacyShot Server for file uploads.

Runs on: Windows 95 (with or without OSR2), 98, ME, NT 4.0, and later,
         including multi-monitor and high-DPI setups on newer Windows.
Needs:   TCP/IP (Winsock 1.1, wsock32.dll)


HOW TO DO IT
------------
LegacyShot sits in the system tray.

1. Press Print Screen (or click the tray icon)
2. Pick what to capture:
     drag              select an area (its size is shown as you drag)
     hover + click     capture the window under the cursor (it is outlined)
     Ctrl + click      capture just the control under the cursor
     Space or Enter    the whole screen (all monitors)
     Esc / right-click cancel (right-click during a drag only drops the drag)
3. The LegacyShot dialog shows a preview. Choose PNG, GIF or BMP, adjust the
   file name or add a description, then:
     Upload     send it to the server; the link is copied to the clipboard
     Save As    write the file to disk in the chosen format
     Copy       copy the picture to the clipboard
     Reselect   make a new selection
4. The upload window shows a progress bar, bytes sent, upload rate, time
   left, and finally the link (Copy Link / Open).

Tray menu (right-click): Capture Area, Capture Whole Screen, Upload Image
File (sends an existing PNG/GIF/BMP/JPEG unchanged), Settings, About, Exit.

Command line: LegacyShot.exe /capture or /full captures at start-up. Running
it again while it is running triggers a capture in the running copy, so a
shortcut with a "Shortcut key" also works as a hotkey. /settings opens Settings.

I recommend putting a shortcut to LegacyShot in your Startup folder, so it's
ready to go when you login.


FORMATS (all encoded on the client)
-----------------------------------
PNG  Lossless. Pictures with 256 colors or less are written as
     palette PNGs at 1, 2, 4 or 8 bits per pixel; others as
     24-bit RGB with per-row adaptive filtering. The deflate
     encoder (LZ77 + dynamic Huffman) is written from RFC 1951.
GIF  Exact when the picture has 256 colors or fewer, otherwise reduced to
     256 with median cut.
BMP  24-bit, uncompressed. Large: a 1024 x 768 capture is 2.3 MB.


RELIABILITY
-----------
- Dropped lines, timeouts and busy servers are retried automatically (up to
  4 attempts); each retry logs in again and resumes from the last chunk the
  server acknowledged.
- Cancel keeps the partial upload on the server for 48 hours. The encoders
  are deterministic, so uploading the same capture again in the same format
  resumes instead of starting over.


BUILDING
--------
Visual C++ 6.0 IDE:  open LegacyShot.dsp, choose Win32 Release, Build.
Command line:        run VC98\Bin\VCVARS32.BAT, then build.bat.
Later Visual Studio: build.bat from an x86 Native Tools prompt also works.

Link libraries: kernel32 user32 gdi32 advapi32 shell32 comctl32 comdlg32
wsock32. The project uses /MT, so LegacyShot.exe can be copied to a bare
Windows 95 machine.
