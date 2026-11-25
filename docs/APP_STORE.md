# App Store submission kit

Everything to paste into App Store Connect. Character limits in brackets.

## App record

| Field | Value |
|---|---|
| Platforms | iOS (iPhone + iPad); optionally macOS (separate archive, same bundle ID = universal purchase) |
| Bundle ID | `com.buyukcaglar.stateoftheart` |
| SKU | `stateoftheart-1` |
| Name [30] | `State of the Art` (16). If taken: `State of the Art: Amiga Demo` (28) |
| Primary language | English (U.S.) |
| Price | Free |
| Primary / secondary category | Entertainment / Music |
| Copyright | `1992 Spaceballs (demo); 2026 Onur Büyükçağlar (app)` |
| Support URL | https://github.com/Buyukcaglar/StateOfTheArt/issues |
| Marketing URL | https://github.com/Buyukcaglar/StateOfTheArt |
| Privacy Policy URL | https://github.com/Buyukcaglar/StateOfTheArt/blob/main/docs/PRIVACY.md |

## Listing

**Subtitle** [30]
```
The 1992 Amiga demo classic
```

**Promotional text** [170]
```
The winner of The Party 1992, now running natively on iPhone, iPad and Mac. Original code, original music, frame-for-frame.
```

**Keywords** [100] (no spaces after commas; "Spaceballs" left out on purpose: it is also a film trademark)
```
amiga,demoscene,demo,retro,1992,a500,chiptune,tracker,mod,music,visual,art,party,classic,oldschool
```

**Description** [4000]
```
In December 1992, a Norwegian group called Spaceballs won the Amiga demo competition at The Party in Denmark with State of the Art: four minutes of dancing silhouettes, pulsing patterns and a pounding soundtrack, squeezed onto a single floppy disk for a 7 MHz Amiga 500. It became one of the best-known demos ever made.

This app is that original party release, running natively. The demo's 68000 machine code was statically recompiled and runs on a model of the Amiga's custom chips, reproducing the demo frame by frame with its original music.

• The complete demo, exactly as released, with its original soundtrack
• Tap to switch between the full picture and full-screen fill; drag to pan when filled
• Plays in portrait or landscape, on iPhone, iPad and Mac
• Loops continuously, a perfect screen to leave running
• Credits and history inside the app
• No ads, no tracking, no data collected, works offline

Credits: code by Lone Starr and Major Asshole, music by Travolta, graphics by TMB Designs.
```

**What's New** (1.0): leave empty for the first version.

## Age rating questionnaire

Everything "None", except **Profanity or Crude Humor: Infrequent/Mild** (one of the authors'
scene names, "Major Asshole", is shown in the credits). No user-generated content, no web
access, no gambling, no contests.

## App Privacy

"Data Not Collected". Matches `platform/apple/PrivacyInfo.xcprivacy` (no tracking,
no collected data; UserDefaults reason CA92.1, system boot time reason 35F9.1).

## Content rights

App Store Connect asks whether the app contains, shows or accesses third-party content, and
if so whether you have the necessary rights. The demo (code, graphics, music) is
third-party content by Spaceballs. Answer this question yourself, truthfully; if Apple asks
for documentation, written permission from the authors is what it will want.

## Export compliance

`ITSAppUsesNonExemptEncryption = NO` is in the Info.plist, so no question at upload.

## Notes for App Review

```
State of the Art is a "demo": a non-interactive audiovisual piece from the Amiga demoscene, the winner of the Amiga demo competition at The Party 1992. The app runs the original release natively (its 68000 code was statically recompiled to C) and plays it with its original soundtrack. It loops after about four minutes.

Interaction:
- Tap: switch between the whole picture (letterboxed) and full-screen fill; an (i) button appears for a few seconds.
- Drag (in fill mode): pan the picture.
- (i) button or long-press anywhere: credits, history and links; the demo pauses while it is open.

No account, no network access, no data collection. Sound plays through the speaker.
```

## Screenshots

`export/appstore/` (local only, not in git; landscape, real captures from a Debug build in the Simulator
using `-SotaFreezeAtFrame N -SotaForceLandscape YES -SotaFillsView YES`):

| Set | Size | Files |
|---|---|---|
| iPhone 6.9" | 2868 × 1320 | `iphone_1_dragon.png` … `iphone_6_tunnel.png` (frames 1800, 2100, 3300, 4200, 10500, 10800) |
| iPad 13" | 2752 × 2064 | `ipad_1_dragon.png` … `ipad_6_tunnel.png`, plus `ipad_7_info.png` (info card) |
| Mac | 2880 × 1800 | not made yet (a capture needs Screen Recording permission) |

The smaller device sizes are scaled from these by App Store Connect.

## Build and upload

```sh
tools/archive_appstore.sh          # archives + exports build/appstore/{ios,macos}
xcrun altool --upload-package build/appstore/ios/*.ipa ...   # or drag into Transporter
```
