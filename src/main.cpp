#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "config.h"
#include "songs.h"

// ---------- state ----------
enum State { IDLE, PLAYING };
State state = IDLE;
unsigned long stateEnteredAt = 0;
unsigned long lastWifiCheck = 0;

// ---------- note frequency helper ----------
static const uint16_t NOTE_FREQS[12] = { 262,277,294,311,330,349,370,392,415,440,466,494 }; // C4..B4

uint16_t noteFreq(uint8_t semitone, uint8_t octave) {
    uint16_t f = NOTE_FREQS[semitone % 12];
    if (octave > 4) f <<= (octave - 4);
    else if (octave < 4) f >>= (4 - octave);
    return f;
}

static uint8_t letterToSemitone(char c) {
    switch (c) {
        case 'c': return 0;  case 'd': return 2;  case 'e': return 4;
        case 'f': return 5;  case 'g': return 7;  case 'a': return 9;
        case 'b': return 11; default:  return 0;
    }
}

// ---------- RTTTL parser ----------
uint16_t parseRTTTL(const char* rtttl, uint16_t out[][2], uint16_t maxNotes) {
    const char* p = rtttl;
    while (*p && *p != ':') p++;
    if (!*p) return 0;
    p++;

    uint8_t defDur = 4, defOct = 6;
    uint16_t bpm = 63;
    while (*p && *p != ':') {
        while (*p == ' ' || *p == ',') p++;
        if (*p == 'd' && *(p+1) == '=') { p += 2; defDur = atoi(p); while (*p >= '0' && *p <= '9') p++; }
        else if (*p == 'o' && *(p+1) == '=') { p += 2; defOct = atoi(p); while (*p >= '0' && *p <= '9') p++; }
        else if (*p == 'b' && *(p+1) == '=') { p += 2; bpm = atoi(p); while (*p >= '0' && *p <= '9') p++; }
        else p++;
    }
    if (!*p) return 0;
    p++;

    if (bpm == 0) bpm = 63;
    uint32_t wholeNote = (60000UL * 4) / bpm;
    uint16_t count = 0;

    while (*p && count < maxNotes) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        uint8_t dur = 0;
        while (*p >= '0' && *p <= '9') { dur = dur * 10 + (*p - '0'); p++; }
        if (dur == 0) dur = defDur;

        uint16_t freq = 0;
        if (*p == 'p' || *p == 'P') {
            p++;
        } else if ((*p >= 'a' && *p <= 'g') || (*p >= 'A' && *p <= 'G')) {
            char note = *p | 0x20;
            p++;
            uint8_t semi = letterToSemitone(note);
            if (*p == '#') { semi++; p++; }
            else if (*p == '_') { semi++; p++; }
            uint8_t oct = defOct;
            if (*p >= '0' && *p <= '9') { oct = *p - '0'; p++; }
            freq = noteFreq(semi, oct);
        } else {
            p++; continue;
        }

        uint16_t ms = wholeNote / dur;
        if (*p == '.') { ms = ms + ms / 2; p++; }

        out[count][0] = freq;
        out[count][1] = ms;
        count++;
    }
    return count;
}

// ---------- MML parser ----------
uint16_t parseMML(const char* mml, uint16_t out[][2], uint16_t maxNotes, uint8_t track = 0) {
    const char* p = mml;

    if (p[0]=='M'&&p[1]=='M'&&p[2]=='L'&&p[3]=='@') p += 4;

    const char* end = p;
    while (*end && *end != ';') end++;

    uint8_t currentTrack = 0;
    while (currentTrack < track && p < end) {
        if (*p == ',') { currentTrack++; if (currentTrack == track) { p++; break; } }
        p++;
    }
    if (currentTrack != track) return 0;

    const char* trackEnd = p;
    while (trackEnd < end && *trackEnd != ',') trackEnd++;

    uint8_t octave = 4;
    uint8_t defaultLength = 4;
    uint16_t tempo = 120;
    uint16_t count = 0;

    while (p < trackEnd && count < maxNotes) {
        char c = *p;

        if (c == 't' || c == 'T') {
            p++;
            uint16_t val = 0;
            while (p < trackEnd && *p >= '0' && *p <= '9') { val = val*10 + (*p-'0'); p++; }
            if (val > 0) tempo = val;
            continue;
        }
        if (c == 'l' || c == 'L') {
            p++;
            uint8_t val = 0;
            while (p < trackEnd && *p >= '0' && *p <= '9') { val = val*10 + (*p-'0'); p++; }
            if (val > 0) defaultLength = val;
            continue;
        }
        if (c == 'o' || c == 'O') {
            p++;
            uint8_t val = 0;
            while (p < trackEnd && *p >= '0' && *p <= '9') { val = val*10 + (*p-'0'); p++; }
            octave = val;
            continue;
        }
        if (c == '>') { octave++; p++; continue; }
        if (c == '<') { octave--; p++; continue; }
        if (c == 'v' || c == 'V') {
            p++;
            while (p < trackEnd && *p >= '0' && *p <= '9') p++;
            continue;
        }

        bool isNote = (c >= 'a' && c <= 'g') || (c >= 'A' && c <= 'G');
        bool isRest = (c == 'r' || c == 'R');

        if (!isNote && !isRest) { p++; continue; }

        uint16_t freq = 0;
        if (isNote) {
            char noteLower = c | 0x20;
            p++;
            uint8_t semi = letterToSemitone(noteLower);
            if (p < trackEnd && (*p == '+' || *p == '#')) { semi++; p++; }
            else if (p < trackEnd && *p == '-') { semi--; p++; }
            freq = noteFreq(semi, octave);
        } else {
            p++;
        }

        uint8_t noteLen = 0;
        while (p < trackEnd && *p >= '0' && *p <= '9') { noteLen = noteLen*10 + (*p-'0'); p++; }
        if (noteLen == 0) noteLen = defaultLength;

        uint32_t wholeNote = (60000UL * 4) / tempo;
        uint32_t ms = wholeNote / noteLen;

        if (p < trackEnd && *p == '.') { ms = ms + ms/2; p++; }

        while (p < trackEnd && *p == '&') {
            p++;
            if (p < trackEnd && ((*p >= 'a' && *p <= 'g') || (*p >= 'A' && *p <= 'G'))) {
                p++;
                if (p < trackEnd && (*p == '+' || *p == '#' || *p == '-')) p++;
            } else if (p < trackEnd && (*p == 'r' || *p == 'R')) {
                p++;
            }
            uint8_t tieLen = 0;
            while (p < trackEnd && *p >= '0' && *p <= '9') { tieLen = tieLen*10 + (*p-'0'); p++; }
            if (tieLen == 0) tieLen = defaultLength;
            ms += wholeNote / tieLen;
            if (p < trackEnd && *p == '.') { ms += (wholeNote / tieLen) / 2; p++; }
        }

        if (ms > 65535) ms = 65535;
        out[count][0] = freq;
        out[count][1] = (uint16_t)ms;
        count++;
    }
    return count;
}

// ---------- multi-track song data model ----------
struct TrackData {
    uint16_t (*notes)[2];
    uint16_t length;
};

struct SongEntry {
    const char* progmemStr;
    const char* name;
    SongFmt fmt;
    uint8_t trackCount;
    TrackData tracks[4];
    bool parsed;
};

#define MAX_SONGS 384
static SongEntry songs[MAX_SONGS];
static uint16_t SONG_COUNT = 0;

uint8_t countMMLTracks(const char* mml) {
    const char* p = mml;
    if (p[0]=='M'&&p[1]=='M'&&p[2]=='L'&&p[3]=='@') p += 4;

    const char* end = p;
    while (*end && *end != ';') end++;

    uint8_t count = 1;
    while (p < end) {
        if (*p == ',') count++;
        p++;
    }
    return count;
}

void freeSongTracks(SongEntry& song) {
    if (!song.parsed) return;
    for (uint8_t t = 0; t < 4; t++) {
        if (song.tracks[t].notes) {
            free(song.tracks[t].notes);
            song.tracks[t].notes = nullptr;
            song.tracks[t].length = 0;
        }
    }
    song.parsed = false;
}

bool parseSongTracks(uint16_t songIdx) {
    if (songIdx >= SONG_COUNT) return false;
    SongEntry& song = songs[songIdx];
    if (song.parsed) return true;

    static const size_t STR_BUF_SZ = 6144;
    char* strBuf = (char*)malloc(STR_BUF_SZ);
    if (!strBuf) { Serial.println("[PARSE] malloc failed"); return false; }

    size_t len = strlen_P(song.progmemStr);
    if (len >= STR_BUF_SZ) {
        Serial.printf("[PARSE] Song too long (%d)\n", len);
        free(strBuf);
        return false;
    }
    strncpy_P(strBuf, song.progmemStr, STR_BUF_SZ - 1);
    strBuf[STR_BUF_SZ - 1] = '\0';

    uint16_t tempBuf[MAX_NOTES_PER_SONG][2];

    if (song.fmt == FMT_RTTTL) {
        uint16_t count = parseRTTTL(strBuf, tempBuf, MAX_NOTES_PER_SONG);
        if (count == 0) { free(strBuf); return false; }
        uint16_t (*notes)[2] = (uint16_t(*)[2])malloc(count * sizeof(uint16_t[2]));
        if (!notes) { free(strBuf); return false; }
        memcpy(notes, tempBuf, count * sizeof(uint16_t[2]));
        song.tracks[0] = { notes, count };
        for (uint8_t t = 1; t < 4; t++) song.tracks[t] = { nullptr, 0 };
    } else {
        uint8_t tracksToparse = song.trackCount < 4 ? song.trackCount : 4;
        for (uint8_t t = 0; t < tracksToparse; t++) {
            uint16_t count = parseMML(strBuf, tempBuf, MAX_NOTES_PER_SONG, t);
            if (count == 0) {
                song.tracks[t] = { nullptr, 0 };
                continue;
            }
            uint16_t (*notes)[2] = (uint16_t(*)[2])malloc(count * sizeof(uint16_t[2]));
            if (!notes) {
                song.tracks[t] = { nullptr, 0 };
                continue;
            }
            memcpy(notes, tempBuf, count * sizeof(uint16_t[2]));
            song.tracks[t] = { notes, count };
        }
        for (uint8_t t = tracksToparse; t < 4; t++) song.tracks[t] = { nullptr, 0 };
    }

    free(strBuf);
    song.parsed = true;
    return true;
}

void parseSongDefs() {
    static const size_t STR_BUF_SZ = 6144;
    char* strBuf = (char*)malloc(STR_BUF_SZ);
    if (!strBuf) { Serial.println("[SONGS] malloc failed"); return; }

    for (uint16_t i = 0; i < SONG_DEF_COUNT && SONG_COUNT < MAX_SONGS; i++) {
        SongDef def;
        memcpy_P(&def, &songDefs[i], sizeof(SongDef));

        char nameBuf[64];
        strncpy_P(nameBuf, def.name, sizeof(nameBuf));
        nameBuf[sizeof(nameBuf)-1] = '\0';

        uint8_t tc = 1;
        if (def.fmt == FMT_MML) {
            size_t len = strlen_P(def.str);
            if (len < STR_BUF_SZ) {
                strncpy_P(strBuf, def.str, STR_BUF_SZ - 1);
                strBuf[STR_BUF_SZ - 1] = '\0';
                tc = countMMLTracks(strBuf);
            }
        }

        songs[SONG_COUNT].progmemStr = def.str;
        songs[SONG_COUNT].name = strdup(nameBuf);
        songs[SONG_COUNT].fmt = def.fmt;
        songs[SONG_COUNT].trackCount = tc;
        songs[SONG_COUNT].parsed = false;
        for (uint8_t t = 0; t < 4; t++) songs[SONG_COUNT].tracks[t] = { nullptr, 0 };
        SONG_COUNT++;
    }

    free(strBuf);
    Serial.printf("[SONGS] Loaded %d song defs\n", SONG_COUNT);
}

// ---------- multi-track melody player ----------
static const uint8_t buzzerPins[NUM_BUZZERS] = { PIN_BUZ0, PIN_BUZ1, PIN_BUZ2, PIN_BUZ3 };

struct MelodyPlayer {
    const uint16_t (*melody)[2];
    uint16_t length;
    uint16_t noteIndex;
    unsigned long noteStartedAt;
    uint16_t gapDuration;
    bool playing;
    bool inGap;
    bool inLoopPause;
    uint8_t buzzerPin;
    uint8_t ledcChannel;
    int8_t octaveShift;
};

MelodyPlayer players[NUM_BUZZERS];
int16_t currentSongIndex = -1;

void playCurrentNote(MelodyPlayer& p) {
    uint16_t freq = p.melody[p.noteIndex][0];
    uint16_t duration = p.melody[p.noteIndex][1];
    if (freq > 0) {
        if (p.octaveShift > 0) {
            for (int8_t i = 0; i < p.octaveShift; i++) freq *= 2;
        } else if (p.octaveShift < 0) {
            for (int8_t i = 0; i < -p.octaveShift; i++) freq /= 2;
        }
        if (freq < 131) freq = 131;
        if (freq > 4000) freq = 4000;

        ledcAttachPin(p.buzzerPin, p.ledcChannel);
        ledcWriteTone(p.ledcChannel, freq);
        p.gapDuration = duration / 10;
        if (p.gapDuration < 20) p.gapDuration = 20;
        if (p.gapDuration >= duration) p.gapDuration = 0;
    } else {
        ledcDetachPin(p.buzzerPin);
        p.gapDuration = 0;
    }
    p.noteStartedAt = millis();
    p.inGap = false;
    p.inLoopPause = false;
}

bool advanceNote(MelodyPlayer& p) {
    p.noteIndex++;
    if (p.noteIndex >= p.length) {
        p.inLoopPause = true;
        p.noteStartedAt = millis();
        ledcDetachPin(p.buzzerPin);
        return false;
    }
    playCurrentNote(p);
    return true;
}

void updatePlayer(MelodyPlayer& p) {
    if (!p.playing) return;
    unsigned long now = millis();

    if (p.inLoopPause) {
        return;
    }

    if (p.inGap) {
        if (now - p.noteStartedAt >= p.gapDuration) {
            advanceNote(p);
        }
        return;
    }

    uint16_t duration = p.melody[p.noteIndex][1];
    uint16_t toneDuration = (p.gapDuration > 0)
        ? (duration - p.gapDuration) : duration;

    if (now - p.noteStartedAt < toneDuration) return;

    if (p.gapDuration > 0) {
        ledcDetachPin(p.buzzerPin);
        p.inGap = true;
        p.noteStartedAt = now;
    } else {
        advanceNote(p);
    }
}

// ---------- track distribution ----------
void assignTracks(SongEntry& song) {
    uint8_t available = 0;
    for (uint8_t t = 0; t < 4; t++) {
        if (song.tracks[t].notes && song.tracks[t].length > 0) available++;
    }

    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        players[i].buzzerPin = buzzerPins[i];
        players[i].ledcChannel = i;
        players[i].octaveShift = 0;
        players[i].melody = nullptr;
        players[i].length = 0;
        players[i].playing = false;
    }

    if (available == 0) return;

    if (available == 1) {
        TrackData& t0 = song.tracks[0];
        players[0].melody = t0.notes; players[0].length = t0.length;
        players[0].octaveShift = 0;
        players[1].melody = t0.notes; players[1].length = t0.length;
        players[1].octaveShift = 1;
        players[2].melody = t0.notes; players[2].length = t0.length;
        players[2].octaveShift = -1;
        players[3].melody = t0.notes; players[3].length = t0.length;
        players[3].octaveShift = 0;
    } else {
        uint8_t assigned = 0;
        for (uint8_t t = 0; t < 4 && assigned < NUM_BUZZERS; t++) {
            if (song.tracks[t].notes && song.tracks[t].length > 0) {
                players[assigned].melody = song.tracks[t].notes;
                players[assigned].length = song.tracks[t].length;
                players[assigned].octaveShift = 0;
                assigned++;
            }
        }
        TrackData& t0 = song.tracks[0];
        if (t0.notes && t0.length > 0) {
            int8_t shifts[] = { 1, -1 };
            uint8_t shiftIdx = 0;
            while (assigned < NUM_BUZZERS) {
                players[assigned].melody = t0.notes;
                players[assigned].length = t0.length;
                players[assigned].octaveShift = shifts[shiftIdx % 2];
                shiftIdx++;
                assigned++;
            }
        }
    }

    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        if (players[i].melody && players[i].length > 0) {
            players[i].noteIndex = 0;
            players[i].playing = true;
            players[i].inGap = false;
            players[i].inLoopPause = false;
            playCurrentNote(players[i]);
        }
    }
}

void stopAllBuzzers() {
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        players[i].playing = false;
        ledcDetachPin(buzzerPins[i]);
    }
}

bool allPlayersInLoopPause() {
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        if (!players[i].playing) continue;
        if (!players[i].inLoopPause) return false;
    }
    return true;
}

bool anyPlayerActive() {
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        if (players[i].playing) return true;
    }
    return false;
}

// ---------- server ----------
AsyncWebServer server(SERVER_PORT);
AsyncWebSocket ws("/ws");

// ---------- PWA ----------
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="theme-color" content="#0f0f0f">
<link rel="manifest" href="/manifest.json">
<link rel="apple-touch-icon" href="/icon.svg">
<title>Music Buzzer</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#0f0f0f;--card:#1a1a1a;--border:#2a2a2a;--text:#e0e0e0;--dim:#666;
--accent:#6c63ff;--accent2:#4ecdc4;--danger:#e53e3e;--success:#38a169}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);
-webkit-user-select:none;user-select:none;overflow-x:hidden;padding-bottom:80px}
header{padding:16px 20px 0;display:flex;align-items:center;justify-content:space-between}
header h1{font-size:1.1rem;font-weight:600;letter-spacing:-0.02em}
.dot{width:8px;height:8px;border-radius:50%;background:var(--danger);transition:background .3s}
.dot.ok{background:var(--success)}
.now-playing{padding:8px 20px;color:var(--dim);font-size:0.8rem;min-height:1.6em;
transition:color .3s}
.now-playing.active{color:var(--accent2)}
.buzzers{display:flex;gap:8px;padding:4px 20px 12px;align-items:center}
.buzzers span{font-size:0.7rem;color:var(--dim);margin-right:4px}
.buz{width:10px;height:10px;border-radius:50%;background:var(--border);transition:background .3s,box-shadow .3s}
.buz.on{background:var(--accent2);box-shadow:0 0 8px var(--accent2)}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
.buz.on{animation:pulse 0.6s ease-in-out infinite}
.songs{list-style:none;padding:0 12px}
.songs li{display:flex;align-items:center;gap:10px;padding:12px;margin-bottom:2px;
border-radius:8px;cursor:pointer;-webkit-tap-highlight-color:transparent;transition:background .15s}
.songs li:active{background:var(--card)}
.songs .play-btn{width:28px;height:28px;border-radius:50%;background:var(--card);border:1px solid var(--border);
display:flex;align-items:center;justify-content:center;flex-shrink:0;transition:background .15s,border-color .15s}
.songs li:active .play-btn{background:var(--accent);border-color:var(--accent)}
.songs .play-btn svg{width:12px;height:12px;fill:var(--dim)}
.songs li:active .play-btn svg{fill:#fff}
.songs .idx{color:var(--dim);font-size:0.7rem;min-width:20px;text-align:right;font-variant-numeric:tabular-nums}
.songs .name{flex:1;font-size:0.9rem;line-height:1.3}
.songs .badge{font-size:0.65rem;color:var(--dim);background:var(--card);border:1px solid var(--border);
padding:1px 6px;border-radius:10px;white-space:nowrap;flex-shrink:0}
.stop-bar{position:fixed;bottom:0;left:0;right:0;padding:12px 20px;
padding-bottom:max(12px,env(safe-area-inset-bottom));
background:linear-gradient(transparent,var(--bg) 20%);display:flex;justify-content:center}
.stop-btn{width:100%;max-width:400px;padding:14px;border:none;border-radius:12px;
background:var(--danger);color:#fff;font-size:1rem;font-weight:600;cursor:pointer;
touch-action:manipulation;-webkit-tap-highlight-color:transparent;letter-spacing:0.02em;
transition:opacity .15s}
.stop-btn:active{opacity:.8}
</style>
</head>
<body>
<header>
<h1>Music Buzzer</h1>
<div class="dot" id="dot"></div>
</header>
<div class="now-playing" id="now">Ready</div>
<div class="buzzers">
<span>Buzzers</span>
<div class="buz" id="b0"></div>
<div class="buz" id="b1"></div>
<div class="buz" id="b2"></div>
<div class="buz" id="b3"></div>
</div>
<ul class="songs" id="list"></ul>
<div class="stop-bar">
<button class="stop-btn" id="stop">STOP</button>
</div>
<script>
var sock=null,connected=false,rTimer=null,songs=[],playing=false;
var dot=document.getElementById('dot');
var list=document.getElementById('list');
var now=document.getElementById('now');
var buzEls=[document.getElementById('b0'),document.getElementById('b1'),
            document.getElementById('b2'),document.getElementById('b3')];
var SERVER=window.location.hostname;

function ui(){
  dot.className=connected?'dot ok':'dot';
}
function setBuzzers(on){
  for(var i=0;i<4;i++) buzEls[i].className=on?'buz on':'buz';
}
function reconnect(){if(!rTimer)rTimer=setTimeout(function(){rTimer=null;connect();},3000);}
function connect(){
  if(sock){sock.onopen=sock.onclose=sock.onerror=sock.onmessage=null;try{sock.close();}catch(e){}}
  try{sock=new WebSocket('ws://'+SERVER+'/ws');}catch(e){reconnect();return;}
  sock.onopen=function(){connected=true;ui();};
  sock.onclose=function(){connected=false;ui();reconnect();};
  sock.onerror=function(){connected=false;ui();reconnect();};
  sock.onmessage=function(e){
    if(e.data.startsWith('playing:')){
      playing=true;
      now.textContent='Now Playing: '+e.data.substring(8);
      now.className='now-playing active';
      setBuzzers(true);
    } else if(e.data==='stopped'){
      playing=false;
      now.textContent='Ready';
      now.className='now-playing';
      setBuzzers(false);
    }
  };
}
function mkPlayBtn(){
  var d=document.createElement('div');
  d.className='play-btn';
  var s=document.createElementNS('http://www.w3.org/2000/svg','svg');
  s.setAttribute('viewBox','0 0 24 24');
  var p=document.createElementNS('http://www.w3.org/2000/svg','polygon');
  p.setAttribute('points','8,5 19,12 8,19');
  s.appendChild(p);
  d.appendChild(s);
  return d;
}
function play(i){
  if(!sock||sock.readyState!==1)return;
  sock.send('play:'+i);
}
document.getElementById('stop').addEventListener('click',function(){
  if(sock&&sock.readyState===1)sock.send('stop');
});
document.addEventListener('visibilitychange',function(){
  if(!document.hidden&&(!sock||sock.readyState!==1)){connected=false;ui();reconnect();}
});
fetch('/songs.json').then(function(r){return r.json();}).then(function(data){
  songs=data;
  data.forEach(function(s){
    var li=document.createElement('li');
    li.appendChild(mkPlayBtn());
    var idx=document.createElement('span');
    idx.className='idx';
    idx.textContent=s.i;
    li.appendChild(idx);
    var nm=document.createElement('span');
    nm.className='name';
    nm.textContent=s.n;
    li.appendChild(nm);
    var badge=document.createElement('span');
    badge.className='badge';
    badge.textContent=s.t+'T';
    li.appendChild(badge);
    li.addEventListener('click',function(){play(s.i);});
    list.appendChild(li);
  });
});
connect();ui();
</script>
</body>
</html>
)rawliteral";

static const char MANIFEST_JSON[] PROGMEM = R"rawliteral(
{"name":"Music Buzzer","short_name":"MusicBuzz","start_url":"/",
"display":"standalone","background_color":"#0f0f0f","theme_color":"#0f0f0f",
"icons":[{"src":"/icon.svg","sizes":"any","type":"image/svg+xml"}]}
)rawliteral";

static const char ICON_SVG[] PROGMEM = R"rawliteral(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 180 180">
<rect width="180" height="180" rx="36" fill="#6c63ff"/>
<text x="90" y="126" font-size="100" font-family="sans-serif" font-weight="bold"
 text-anchor="middle" fill="#fff">&#9835;</text>
</svg>
)rawliteral";

// ---------- state management ----------
void enterState(State s);

void startSong(uint16_t index) {
    if (currentSongIndex >= 0 && currentSongIndex < SONG_COUNT) {
        freeSongTracks(songs[currentSongIndex]);
    }

    if (!parseSongTracks(index)) {
        Serial.printf("[PLAY] Failed to parse song #%d\n", index);
        return;
    }

    currentSongIndex = index;
    assignTracks(songs[index]);

    Serial.printf("[PLAY] Starting: %s (%d tracks)\n",
        songs[index].name, songs[index].trackCount);
}

void enterState(State s) {
    state = s;
    stateEnteredAt = millis();

    switch (s) {
    case IDLE:
        stopAllBuzzers();
        ws.textAll("stopped");
        break;
    case PLAYING:
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "playing:%s", songs[currentSongIndex].name);
            ws.textAll(msg);
        }
        break;
    }
}

// ---------- WebSocket handler ----------
void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
    switch (type) {
    case WS_EVT_CONNECT:
        Serial.printf("[WS] Client #%u connected\n", client->id());
        if (state == PLAYING && currentSongIndex >= 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "playing:%s", songs[currentSongIndex].name);
            client->text(msg);
        }
        break;
    case WS_EVT_DISCONNECT:
        Serial.printf("[WS] Client #%u disconnected\n", client->id());
        break;
    case WS_EVT_DATA: {
        AwsFrameInfo* info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            if (len == 4 && memcmp(data, "stop", 4) == 0 && state == PLAYING) {
                Serial.println("[WS] Stop received");
                enterState(IDLE);
            } else if (len >= 6 && len <= 9 && memcmp(data, "play:", 5) == 0) {
                char numBuf[8];
                memcpy(numBuf, data + 5, len - 5);
                numBuf[len - 5] = '\0';
                int idx = atoi(numBuf);
                if (idx >= 0 && idx < SONG_COUNT) {
                    if (state == PLAYING) {
                        stopAllBuzzers();
                    }
                    startSong(idx);
                    enterState(PLAYING);
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

// ---------- setup ----------
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[BOOT] Music Buzzer starting...");

    // LEDC init — each buzzer on its own channel (channels 0-3 map to timers 0-3)
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        ledcSetup(i, 1000, LEDC_RESOLUTION);
        ledcAttachPin(buzzerPins[i], i);
        ledcDetachPin(buzzerPins[i]);
    }

    // Stop button
    pinMode(PIN_STOP_BTN, INPUT_PULLUP);

    // WiFi
    IPAddress ip(BUZZER_IP);
    IPAddress gw(GATEWAY);
    IPAddress sn(SUBNET);
    WiFi.config(ip, gw, sn);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[WIFI] Connecting");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WIFI] Connected — IP: %s\n", WiFi.localIP().toString().c_str());

    // Parse song definitions (lazy — names + track counts only)
    parseSongDefs();
    Serial.printf("[HEAP] Free after song defs: %u bytes\n", ESP.getFreeHeap());

    // WebSocket
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);

    // HTTP routes
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(200, "text/html", INDEX_HTML);
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        request->send(response);
    });
    server.on("/manifest.json", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "application/json", MANIFEST_JSON);
    });
    server.on("/icon.svg", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(200, "image/svg+xml", ICON_SVG);
    });
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->redirect("/icon.svg");
    });
    server.on("/songs.json", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->print("[");
        for (uint16_t i = 0; i < SONG_COUNT; i++) {
            if (i > 0) response->print(",");
            response->printf("{\"i\":%d,\"n\":\"", i);
            const char* n = songs[i].name;
            while (*n) {
                if (*n == '"') response->print("\\\"");
                else if (*n == '\\') response->print("\\\\");
                else response->print(*n);
                n++;
            }
            uint8_t tc = songs[i].trackCount;
            if (tc > 4) tc = 4;
            response->printf("\",\"t\":%d}", tc);
        }
        response->print("]");
        request->send(response);
    });
    server.onNotFound([](AsyncWebServerRequest* request) {
        request->send(404, "text/plain", "Not found");
    });

    server.begin();
    Serial.printf("[BOOT] Server started — %d songs loaded\n", SONG_COUNT);
}

// ---------- loop ----------
void loop() {
    // Stop button — sustained-LOW 30ms debounce
    {
        static bool lastStableState = HIGH;
        static bool lastReading = HIGH;
        static unsigned long lastChangeAt = 0;

        bool reading = digitalRead(PIN_STOP_BTN);
        unsigned long now = millis();

        if (reading != lastReading) {
            lastChangeAt = now;
        }
        lastReading = reading;

        if (now - lastChangeAt >= 30 && reading != lastStableState) {
            lastStableState = reading;
            if (reading == LOW && state == PLAYING) {
                Serial.println("[BTN] Stop pressed");
                enterState(IDLE);
            }
        }
    }

    ws.cleanupClients();

    // Update all players
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        updatePlayer(players[i]);
    }

    // Synchronized looping: when all active players are in loop pause, restart together
    if (state == PLAYING && anyPlayerActive() && allPlayersInLoopPause()) {
        unsigned long now = millis();
        unsigned long latestPause = 0;
        for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
            if (players[i].playing && players[i].inLoopPause) {
                unsigned long elapsed = now - players[i].noteStartedAt;
                if (elapsed < latestPause || latestPause == 0) latestPause = elapsed;
            }
        }
        if (latestPause >= MELODY_LOOP_PAUSE_MS) {
            for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
                if (players[i].playing) {
                    players[i].noteIndex = 0;
                    playCurrentNote(players[i]);
                }
            }
        }
    }

    // Auto-stop if no players are active
    if (state == PLAYING) {
        if (!anyPlayerActive() && millis() - stateEnteredAt >= STATE_SETTLE_MS) {
            Serial.println("[PLAY] No active players, stopping");
            enterState(IDLE);
        }
    }

    // WiFi reconnect
    if (millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WIFI] Reconnecting...");
            WiFi.reconnect();
        }
    }
}
