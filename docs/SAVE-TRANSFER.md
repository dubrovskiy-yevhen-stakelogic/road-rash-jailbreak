# PC and Quest save transfer

Install the included Quest APK (package `com.rrjb.vr`). Desktop and PCVR share the same PC saves.

Both platforms keep the career in the same file, a raw 128 KiB PlayStation memory card image, so no
conversion is needed:

- PC: `<game folder>\saves\rrjb_card.mcr`
- Quest: `files/saves/rrjb_card.mcr` in the app's internal storage, accessed by the USB-only save provider
  at `content://com.rrjb.vr.saves`. It works in non-debuggable release builds, exposes only the card and
  refuses access while the game is running. Uploads are validated and atomically committed only if the
  destination still matches its original SHA-256. The previous card is also retained on the headset.

1. Save in the game's own memory-card menu, then close the game on both devices.
2. Connect the Quest over USB with developer mode enabled and accept USB debugging.
3. Run **TRANSFER_QUEST_SAVES_TO_PC.bat** or **TRANSFER_PC_SAVES_TO_QUEST.bat** and confirm the PC game
   folder. The tool finds ADB (see [Quest](QUEST.md)), shows the plan and asks before replacing.

The source card must be a valid card (128 KiB, `MC` header, every directory frame's checksum, at least
one used block); empty or damaged cards are refused. Identical cards are skipped. Nothing is merged:
the destination card is replaced by the source card. Both cards are first copied to
`<game folder>\save-backups\<timestamp-id>\` (`career-source.mcr`, `career-destination.mcr`), and the result is read back and
compared by SHA-256 on both platforms. The transfer refuses to run while `rrgame.exe` is
running on the PC. Settings and the installed disc are not transferred.

```powershell
.\scripts\transfer-saves.ps1 -Direction QuestToPC -Runtime 'D:\Games\RoadRashJailbreak'
.\scripts\transfer-saves.ps1 -Direction PCToQuest -Runtime 'D:\Games\RoadRashJailbreak' -Serial YOUR_SERIAL -Yes
```

To restore a PC backup, close the game and copy the backup's `career-destination.mcr` (from a Quest-to-PC transfer) over `saves\rrjb_card.mcr`.
