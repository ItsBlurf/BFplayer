# VLC-compatible display modes

BFplayer keeps resizing, display-aspect override, and source cropping
as three separate operations. This matches VLC Desktop's distinction between
the **Aspect Ratio** and **Crop** menus and prevents a 4:3 crop from silently
becoming a distorted 16:9 stretch.

Reference:
[VLC Desktop 3.0 video documentation](https://docs.videolan.me/vlc-user/desktop/3.0/en/basic/video.html).

## Resize modes

| Mode | Behavior |
| --- | --- |
| Best fit | Preserve the effective video ratio and show the whole picture with letterbox/pillarbox bars as needed. |
| Fill screen (crop) | Preserve the effective ratio, center-crop the minimum amount, and cover the full 1920×1080 output. |
| Fullscreen stretch | Map the selected source rectangle to all 1920×1080 pixels; this may distort it. |

## Display-aspect overrides

These use the complete selected source rectangle but tell the renderer to
display it at an exact ratio. A non-native override can therefore deliberately
stretch or squeeze the picture, just as VLC's Aspect Ratio command does.

```text
Default
1:1
4:3
16:9
16:10
2.21:1
2.35:1
2.39:1
5:4
```

## Crop ratios

These remove pixels symmetrically from the decoded source to produce the exact
requested ratio. They do not distort the remaining picture.

```text
Default
16:10
16:9
1.85:1
2.21:1
2.35:1
2.39:1
5:3
4:3
5:4
1:1
```

The implementation honors the stream's display aspect ratio, including
anamorphic pixel-aspect correction. Crop is applied first, aspect override
second, and resize mode last. All three settings persist globally in SQLite.

## Controller controls

| Combination | Action |
| --- | --- |
| Triangle | Cycle decoded video tracks |
| R2 + Triangle | Cycle display-aspect override |
| L2 + Triangle | Cycle crop ratio |
| L2 + R2 + Triangle | Cycle Best fit, Fill screen, and Fullscreen stretch |

The on-screen display names the resulting selection after every change.
