---
{
  "name": "radio_player",
  "title": "网络\n收音机",
  "description": "Internet radio player with station presets, ICY song title, and volume control",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "manage_mode": "readonly"
  },
  "execution": {
    "entry": "{CUR_SKILL_DIR}/scripts/main.lua",
    "icon": "assets/icon.jpg",
    "visible": true,
    "order": 11
  }
}
---
# 网络收音机 (Radio Player)

An internet radio player that streams live radio stations (HLS / direct MP3 / AAC) over WiFi. Provides a touch-based LVGL interface with:

- **Station list**: preset channels loaded from editable `stations.json` (RTHK Hong Kong + mainland channels)
- **Now playing**: current station, ICY song title (when provided), volume slider, stop control
- **Manual play**: enter any radio stream URL directly

## Usage

1. Launch Radio Player from the launcher
2. Tap a station to start streaming
3. Use the **播放** tab to see the current station, song title (ICY), and volume
4. Use the **手动** tab to enter a custom stream URL

## Station List

Stations are stored in `{CUR_SKILL_DIR}/stations.json` and can be edited freely:

```json
[
  {"name": "香港电台第一台", "url": "https://rthkradio1-live.akamaized.net/hls/live/2035313/radio1/master.m3u8", "type": "hls"},
  {"name": "商业电台 雷霆881", "url": "https://...", "type": "http"}
]
```

- `type: "hls"` → HLS m3u8 playlist (segmented TS)
- `type: "http"` → direct MP3/AAC stream (ICY song title supported)

## Tool Call Inputs

```json
{"path":"{CUR_SKILL_DIR}/scripts/main.lua","args":{}}
```
