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

- **Station list**: preset channels loaded from editable `stations.json` (RTHK Hong Kong + CNR mainland + Shenzhen local channels)
- **Now playing**: current station, ICY song title (when provided), volume slider, stop control
- **Manual play**: enter any radio stream URL directly

## Usage

1. Launch Radio Player from the launcher
2. Tap a station to start streaming
3. Use the **播放** tab to see the current station, song title (ICY), and volume
4. Use the **手动** tab to enter a custom stream URL

## Station List

Stations are stored in `{CUR_SKILL_DIR}/stations.json` and can be edited freely. The default list ships 26 stations:

- **RTHK 香港电台 (8)**: 第一至第五台、普通话台、转播CNR香港之声、转播大湾区之声
- **中央人民广播电台 CNR (13)**: 中国之声/经济之声/音乐之声/经典音乐广播/台海之声/神州之声/大湾区之声/民族之声/文艺之声/老年之声/香港之声/中国交通广播/中国乡村之声 (CDN `ngcdn002.cnr.cn`)
- **深圳本地台 (5)**: 先锋898/飞扬971/快乐1062/私家车94.2/星光FM99.1 (蜻蜓FM `ls.qingting.fm`)

> **未收录说明**: 香港商业电台 (881/903/864) 直播流使用 CloudFront 动态签名 Cookie (Policy/Signature/Key-Pair-Id)，签名与请求 IP 绑定且每次实时生成，无法静态收录；新城电台域名 `metroradio.com.hk` 在大陆网络不可达。两者均需专用 App 或浏览器播放。

```json
[
  {"name": "香港电台第一台", "url": "https://rthkradio1-live.akamaized.net/hls/live/2035313/radio1/master.m3u8", "type": "hls"},
  {"name": "深圳先锋898", "url": "https://ls.qingting.fm/live/1270.m3u8", "type": "hls"}
]
```

- `type: "hls"` → HLS m3u8 playlist (segmented TS)
- `type: "http"` → direct MP3/AAC stream (ICY song title supported)

## Tool Call Inputs

```json
{"path":"{CUR_SKILL_DIR}/scripts/main.lua","args":{}}
```
