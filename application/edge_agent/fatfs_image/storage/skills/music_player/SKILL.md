---
{
  "name": "music_player",
  "title": "音乐\n播放器",
  "description": "Local music player with playlist, volume control, progress bar, and WiFi upload",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "manage_mode": "readonly"
  },
  "execution": {
    "entry": "{CUR_SKILL_DIR}/scripts/main.lua",
    "icon": "assets/icon.jpg",
    "visible": true,
    "order": 10
  }
}
---
# Music Player

Local music player that plays audio files from the SD card `/sdcard/music/` directory. Provides a touch-based LVGL interface with:

- **Now Playing**: current song, progress bar, play/pause/stop/next/prev controls, volume slider
- **Playlist**: scrollable list of audio files scanned from `/sdcard/music/`
- **WiFi Upload**: starts an HTTP upload server for transferring music files over the network

## Usage

1. Copy MP3/WAV/AAC/OGG/FLAC files to SD card's `/music/` folder
2. Launch Music Player from the launcher
3. Tap the **Playlist** tab to see available songs
4. Tap a song to play it
5. Use the **Now Playing** tab for transport controls and volume
6. Use the **Upload** tab to start a WiFi upload server

## Tool Call Inputs

```json
{"path":"{CUR_SKILL_DIR}/scripts/main.lua","args":{}}
```
