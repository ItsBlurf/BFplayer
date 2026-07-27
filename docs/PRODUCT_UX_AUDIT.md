# Product and UX audit

This audit covers the BFplayer library, Add Media flow, playback
controls, settings, diagnostics, and current build inputs.

## Findings fixed for alpha.14 through alpha.16

- Add Media could enumerate a removable exFAT entry but silently discard it
  when descriptor-relative `fstatat` failed. Bulk import now uses the same
  no-follow `lstat` fallback as the proven source browser and scanner, while
  still aborting on fatal I/O and refusing symlinks or device crossings.
- Import notices were appended to a clipped status string. Import failure,
  empty-result, and success messages now have a dedicated visible toast and
  detailed diagnostic log records.
- Square had an unclear scope. It now imports the highlighted directory, or
  the current directory when a file is highlighted, and says which action
  will occur in the footer.
- Whole-library discovery now runs on a cancellable worker with live checked
  entry and source counts, keeping navigation/rendering responsive and
  discarding canceled partial results.
- Options stopped playback without confirmation. It now opens a playback menu
  containing Resume, Controls, Subtitle timing, and Return to library.
- Touchpad enabled a hidden subtitle-delay mode that repurposed every D-pad
  direction. Touchpad now opens the complete playback control map; subtitle
  timing lives visibly in the playback menu, so D-pad always seeks.
- Playback constants were not user-configurable. The library Options menu now
  persists default volume, short/long seek steps, OSD duration, resume, and
  automatic-subtitle behavior.
- The in-player Options menu exposes those settings without leaving the video
  and also provides live scaling, display-aspect, and crop selection, replacing
  shortcut-only discovery for common display adjustments.
- The library repeated dashboard branding and showed decorative empty artwork.
  Runtime branding is now reserved for the installed tile; the app uses a
  simpler title, content-first layout, real generated/local artwork only, and
  a small default footer with the full command set available under Controls.
- The empty library is now a full-width first-run screen where Cross opens Add
  Media directly instead of doing nothing. Empty searches and categories have
  distinct recovery guidance and do not masquerade as a first-run library.
- Search and Clear Search are now available in the controller Options menu;
  Left/Right also move display settings in the direction shown.
- First-run Cross no longer waits for the initial scanner flag to clear before
  opening Add Media. The browser now has its own keyboard route as well as the
  PS5 controller route, so inputs cannot fall through to the hidden library.
- Add Media now carries a compact, width-checked action legend. Cross handles
  a folder or one movie, Triangle adds one TV folder, and Square opts into a
  mixed-library import; the checked-in Add Media preview verifies this layout.
- Source-browser diagnostics now record the complete disposition of a folder
  listing, including visible directories/media, descriptor-stat fallbacks,
  unreadable entries, symlinks, filtered metadata, and non-regular entries.
- A library mutation cannot race an active or pending import. Failed
  persistence/removal paths restart the last-good scan instead of leaving the
  scanner stopped.
- All playback setting fields are now persisted in one SQLite transaction.
  Failed saves roll back on disk and restore the prior live values.
- Mute is reachable from the PS5 playback Options menu. Controller hot-plug
  clears held-button state and updates the controller handle used by playback.
- Fixed-size launcher error text uses bounded formatting.
- Long playback messages, panel titles, and panel rows are fitted to their
  actual pixel bounds without cutting a UTF-8 codepoint.

## Interaction model

The UI follows a predictable controller hierarchy:

1. D-pad changes focus or performs the clearly documented seek action.
2. Cross activates the focused item.
3. Circle backs out one level.
4. Options opens a contextual menu and never performs a destructive or
   surprising immediate exit.
5. Touchpad opens Add Media in the library and the control reference during
   playback.
6. Every non-obvious shortcut is listed in the in-app Controls page.

This is consistent with Apple's controller/TV guidance that focus must remain
visible and navigation predictable:
<https://developer.apple.com/design/human-interface-guidelines/focus-and-selection/>.

## Media-player references

- mpv exposes its complete input map in-player and uses directional short/long
  seek conventions: <https://mpv.io/manual/master/>.
- VLC separates approachable preferences from the full advanced surface and
  groups audio, video, subtitles/OSD, and hotkeys:
  <https://docs.videolan.me/vlc-user/desktop/3.0/en/basic/settings/preferences.html>.
- PlayStation presents attached USB storage as an explicit media source rather
  than silently scanning the whole console:
  <https://www.playstation.com/en-gb/support/hardware/play-video-music-discs-usb-drives/>.
- PS5 accessibility guidance supports visible text, contrast, captions, and
  remappable control expectations:
  <https://www.playstation.com/en-us/support/hardware/ps5-accessibility-settings/>.

## Build-input audit

- The project builds with the locally installed open-source PS5 payload SDK
  release line v0.41:
  <https://github.com/ps5-payload-dev/sdk/releases/tag/v0.41>.
- PacBrew v0.37 is the current packaged dependency release used by this tree:
  <https://github.com/ps5-payload-dev/pacbrew-repo/releases/tag/v0.37>.
- The player remains a usermode PIE payload. No kernel exploit, kernel text
  read, or whole-filesystem automatic scan is introduced.

## Remaining hardware acceptance gates

Offline tests can verify parsing, traversal policy, persistence, packaging,
ELF structure, and launcher behavior. A controller is still required to
confirm the final focus feel and to activate Import Selected Folder on an
actual exFAT USB drive. The resulting diagnostic log must contain either
`bulk-import complete` or an explicit `bulk-import failed/empty` record; it
must never stop after only `bulk-import begin`.
