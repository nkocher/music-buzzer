#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "config.h"
#include "songs.h"
#include "mini_gpt.h"

// ---------- state ----------
enum State { IDLE, PLAYING };
volatile State state = IDLE;
volatile unsigned long stateEnteredAt = 0;
unsigned long lastWifiCheck = 0;
uint8_t volumePercent = DEFAULT_VOLUME;

// ---------- GPT generation ----------
MiniGPT gptModel;
bool gptLoaded = false;
volatile bool generating = false;
volatile bool genAbort = false;
float genTemperature = 0.8f;
QueueHandle_t genResultQueue;
QueueHandle_t wsMessageQueue;  // For thread-safe WS messaging from core 0

// Send a WebSocket message from any core (queued, drained in loop on core 1)
static void queueWsMessage(const char* msg) {
    char* copy = strdup(msg);
    if (copy) {
        if (xQueueSend(wsMessageQueue, &copy, 0) != pdTRUE) {
            free(copy);  // Queue full, drop message
        }
    }
}

// Forward declarations
void playGeneratedMML(char* mml);
void enterState(State s);

// ---------- Software PWM via timer ISR (replaces LEDC to avoid first-cycle glitch) ----------
#define SAMPLE_RATE_HZ 40000
#define TIMER_DIVIDER  2      // 80MHz / 2 = 40MHz, then alarm at 1000 ticks = 40kHz

struct BuzzerPWM {
    volatile uint32_t phase;      // 32-bit phase accumulator
    volatile uint32_t phaseInc;   // Phase increment (determines frequency)
    volatile uint16_t dutyOn;     // PWM duty threshold (0-512)
};
volatile BuzzerPWM buzzerPWM[NUM_BUZZERS] = {};
hw_timer_t* audioTimer = nullptr;

// Pin masks for direct GPIO manipulation (GPIO 4,5,6,7 are on GPIO.out, GPIO 15 is also on GPIO.out)
static const uint32_t buzzerPinMasks[NUM_BUZZERS] = {
    (1UL << PIN_BUZ0), (1UL << PIN_BUZ1), (1UL << PIN_BUZ2),
    (1UL << PIN_BUZ3), (1UL << PIN_BUZ4)
};

void IRAM_ATTR audioISR() {
    uint32_t setMask = 0;
    uint32_t clearMask = 0;
    bool anyActive = false;

    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        if (buzzerPWM[i].phaseInc == 0) {
            continue;  // Don't add to clearMask - pin is already LOW
        }
        anyActive = true;
        buzzerPWM[i].phase += buzzerPWM[i].phaseInc;

        // Upper 9 bits as phase position (0-511)
        uint16_t pos = buzzerPWM[i].phase >> 23;
        if (pos < buzzerPWM[i].dutyOn) {
            setMask |= buzzerPinMasks[i];
        } else {
            clearMask |= buzzerPinMasks[i];
        }
    }

    if (!anyActive) return;  // Early exit - no GPIO writes when idle

    if (setMask) GPIO.out_w1ts = setMask;
    if (clearMask) GPIO.out_w1tc = clearMask;
}

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

        uint32_t divisor = (uint32_t)bpm * dur;
        uint16_t ms = (uint16_t)((240000UL + divisor / 2) / divisor);
        if (*p == '.') { ms = (ms * 3 + 1) / 2; p++; }

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

    // Scan Track 0 preamble for initial tempo (applies to all tracks)
    uint16_t initTempo = 120;
    {
        const char* s = p;
        const char* t0end = s;
        while (t0end < end && *t0end != ',') t0end++;
        while (s < t0end) {
            char c = *s;
            if ((c >= 'a' && c <= 'g') || (c >= 'A' && c <= 'G') || c == 'r' || c == 'R')
                break; // stop at first note/rest
            if (c == 't' || c == 'T') {
                s++;
                uint16_t val = 0;
                while (s < t0end && *s >= '0' && *s <= '9') { val = val*10 + (*s-'0'); s++; }
                if (val > 0) initTempo = val;
            } else {
                s++;
            }
        }
    }

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
    uint16_t tempo = initTempo;
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

        // Single rounded division to avoid double-truncation drift between tracks
        uint32_t divisor = (uint32_t)tempo * noteLen;
        uint32_t ms = (240000UL + divisor / 2) / divisor;

        if (p < trackEnd && *p == '.') { ms = (ms * 3 + 1) / 2; p++; }

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
            uint32_t tieDivisor = (uint32_t)tempo * tieLen;
            uint32_t tieMs = (240000UL + tieDivisor / 2) / tieDivisor;
            if (p < trackEnd && *p == '.') { tieMs = (tieMs * 3 + 1) / 2; p++; }
            ms += tieMs;
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
    TrackData tracks[MAX_TRACKS];
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
    for (uint8_t t = 0; t < MAX_TRACKS; t++) {
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
    Serial.printf("[PARSE] Song #%d: fmt=%d, strLen=%d, tracks=%d\n",
        songIdx, song.fmt, len, song.trackCount);

    uint16_t (*tempBuf)[2] = (uint16_t(*)[2])malloc(MAX_NOTES_PER_SONG * sizeof(uint16_t[2]));
    if (!tempBuf) { free(strBuf); Serial.println("[PARSE] tempBuf malloc failed"); return false; }
    Serial.printf("[PARSE] tempBuf alloc OK, heap=%u\n", ESP.getFreeHeap());

    if (song.fmt == FMT_RTTTL) {
        uint16_t count = parseRTTTL(strBuf, tempBuf, MAX_NOTES_PER_SONG);
        Serial.printf("[PARSE] RTTTL parsed: %d notes\n", count);
        if (count == 0) { free(tempBuf); free(strBuf); return false; }
        uint16_t (*notes)[2] = (uint16_t(*)[2])malloc(count * sizeof(uint16_t[2]));
        if (!notes) { free(tempBuf); free(strBuf); return false; }
        memcpy(notes, tempBuf, count * sizeof(uint16_t[2]));
        song.tracks[0] = { notes, count };
        for (uint8_t t = 1; t < MAX_TRACKS; t++) song.tracks[t] = { nullptr, 0 };
    } else {
        uint8_t tracksToparse = song.trackCount < MAX_TRACKS ? song.trackCount : MAX_TRACKS;
        for (uint8_t t = 0; t < tracksToparse; t++) {
            uint16_t count = parseMML(strBuf, tempBuf, MAX_NOTES_PER_SONG, t);
            Serial.printf("[PARSE] MML track %d: %d notes\n", t, count);
            if (count == 0) {
                song.tracks[t] = { nullptr, 0 };
                continue;
            }
            uint16_t (*notes)[2] = (uint16_t(*)[2])malloc(count * sizeof(uint16_t[2]));
            if (!notes) {
                Serial.printf("[PARSE] Track %d notes malloc failed!\n", t);
                song.tracks[t] = { nullptr, 0 };
                continue;
            }
            memcpy(notes, tempBuf, count * sizeof(uint16_t[2]));
            song.tracks[t] = { notes, count };
        }
        for (uint8_t t = tracksToparse; t < MAX_TRACKS; t++) song.tracks[t] = { nullptr, 0 };
    }

    free(tempBuf);
    free(strBuf);
    song.parsed = true;
    for (uint8_t t = 0; t < MAX_TRACKS; t++) {
        if (song.tracks[t].notes && song.tracks[t].length > 0) {
            uint32_t totalMs = 0;
            for (uint16_t n = 0; n < song.tracks[t].length; n++)
                totalMs += song.tracks[t].notes[n][1];
            Serial.printf("[PARSE] Track %d: %d notes, %ums total\n",
                t, song.tracks[t].length, totalMs);
        }
    }
    Serial.printf("[PARSE] Done, heap=%u\n", ESP.getFreeHeap());
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
        for (uint8_t t = 0; t < MAX_TRACKS; t++) songs[SONG_COUNT].tracks[t] = { nullptr, 0 };
        SONG_COUNT++;
    }

    free(strBuf);
    Serial.printf("[SONGS] Loaded %d song defs\n", SONG_COUNT);
}

// ---------- multi-track melody player ----------
static const uint8_t buzzerPins[NUM_BUZZERS] = { PIN_BUZ0, PIN_BUZ1, PIN_BUZ2, PIN_BUZ3, PIN_BUZ4 };

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

// Set up buzzer output for current note (does NOT touch timing)
void setupNote(MelodyPlayer& p) {
    uint16_t freq = p.melody[p.noteIndex][0];
    uint16_t duration = p.melody[p.noteIndex][1];
    if (freq > 0) {
        // Apply octave shift via bit shifting (each octave doubles/halves frequency)
        if (p.octaveShift > 0) freq <<= p.octaveShift;
        else if (p.octaveShift < 0) freq >>= -p.octaveShift;

        // Clamp to usable range for passive buzzers
        if (freq < 65) freq = 65;
        if (freq > 4000) freq = 4000;

        // Software PWM via timer ISR — phase-continuous, no first-cycle glitch
        buzzerPWM[p.ledcChannel].phase = 0;  // Reset phase for clean note attack
        buzzerPWM[p.ledcChannel].phaseInc = ((uint64_t)freq << 32) / SAMPLE_RATE_HZ;
        buzzerPWM[p.ledcChannel].dutyOn = ((uint32_t)volumePercent * 512) / 100;
        p.gapDuration = duration / 10;
        if (p.gapDuration < 20) p.gapDuration = 20;
        if (p.gapDuration >= duration) p.gapDuration = 0;
    } else {
        buzzerPWM[p.ledcChannel].phaseInc = 0;
        p.gapDuration = 0;
    }
    p.inGap = false;
}

void advanceNote(MelodyPlayer& p) {
    p.noteIndex++;
    if (p.noteIndex >= p.length) {
        Serial.printf("[TRACK] Buzzer %d finished (%d notes) at %lums\n",
            p.ledcChannel, p.length, millis());
        p.inLoopPause = true;
        buzzerPWM[p.ledcChannel].dutyOn = 0;  // Silence via duty cycle
        return;
    }
    setupNote(p);
}

void updatePlayer(MelodyPlayer& p) {
    if (!p.playing || p.inLoopPause) return;
    unsigned long elapsed = millis() - p.noteStartedAt;
    uint16_t duration = p.melody[p.noteIndex][1];
    uint16_t toneDuration = (p.gapDuration > 0)
        ? (duration - p.gapDuration) : duration;

    // Silence buzzer when tone portion ends (gap begins)
    if (!p.inGap && p.gapDuration > 0 && elapsed >= toneDuration) {
        buzzerPWM[p.ledcChannel].dutyOn = 0;  // Silence via duty cycle
        p.inGap = true;
    }

    // Advance to next note when full duration ends (absolute timing)
    if (elapsed >= duration) {
        p.noteStartedAt += duration;
        advanceNote(p);
    }
}

// ---------- track distribution ----------
void assignTracks(SongEntry& song) {
    uint8_t available = 0;
    for (uint8_t t = 0; t < MAX_TRACKS; t++) {
        if (song.tracks[t].notes && song.tracks[t].length > 0) available++;
    }

    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        MelodyPlayer& p = players[i];
        p.buzzerPin = buzzerPins[i];
        p.ledcChannel = i;
        p.octaveShift = 0;
        p.melody = nullptr;
        p.length = 0;
        p.playing = false;
    }

    if (available == 0) return;

    if (available == 1) {
        TrackData& t0 = song.tracks[0];
        // 3 buzzers: base + octave up + octave down for harmonic richness
        static const int8_t shifts[3] = { 0, 1, -1 };
        for (uint8_t i = 0; i < 3; i++) {
            players[i].melody = t0.notes;
            players[i].length = t0.length;
            players[i].octaveShift = shifts[i];
        }
    } else {
        uint8_t assigned = 0;
        for (uint8_t t = 0; t < MAX_TRACKS && assigned < NUM_BUZZERS; t++) {
            if (song.tracks[t].notes && song.tracks[t].length > 0) {
                Serial.printf("[ASSIGN] Buzzer %d: Track %d\n", assigned, t);
                players[assigned].melody = song.tracks[t].notes;
                players[assigned].length = song.tracks[t].length;
                players[assigned].octaveShift = 0;
                assigned++;
            }
        }
    }

    unsigned long startTime = millis();
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        MelodyPlayer& p = players[i];
        if (p.melody && p.length > 0) {
            p.noteIndex = 0;
            p.playing = true;
            p.inGap = false;
            p.inLoopPause = false;
            p.noteStartedAt = startTime;
            setupNote(p);
        }
    }

    // Only set OUTPUT for buzzers that are playing (unused stay in INPUT/hi-Z)
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        if (players[i].playing) {
            pinMode(buzzerPins[i], OUTPUT);
            digitalWrite(buzzerPins[i], LOW);
        }
    }

    // Enable timer ISR now that playback is configured
    timerAlarmEnable(audioTimer);
}

void stopAllBuzzers() {
    // Disable timer ISR first to stop all GPIO activity
    timerAlarmDisable(audioTimer);

    uint32_t allPinsMask = 0;
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        players[i].playing = false;
        buzzerPWM[i].phaseInc = 0;
        buzzerPWM[i].phase = 0;
        buzzerPWM[i].dutyOn = 0;
        allPinsMask |= buzzerPinMasks[i];
    }

    // Force all buzzer pins LOW, then switch to INPUT for true silence
    GPIO.out_w1tc = allPinsMask;
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        pinMode(buzzerPins[i], INPUT);
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

// ---------- GPT streaming callback and generation task ----------
void streamCallback(const char* token, void* userData) {
    if (genAbort) return;
    char msg[64];
    snprintf(msg, sizeof(msg), "gen:t:%s", token);
    queueWsMessage(msg);
}

// Storage for generated song playback (persists until next gen or song play)
static char* genMmlBuf = nullptr;

void playGeneratedMML(char* mml) {
    // Free previous generated song if any
    if (currentSongIndex >= 0 && currentSongIndex < SONG_COUNT) {
        freeSongTracks(songs[currentSongIndex]);
    }

    // Use the last slot in songs[] as a temporary generated song entry
    // This avoids needing a separate SongEntry allocation
    uint16_t genIdx = SONG_COUNT;  // One past the loaded songs
    if (genIdx >= MAX_SONGS) {
        Serial.println("[GPT] No song slot available");
        return;
    }

    // Free old gen buffer
    if (genMmlBuf) {
        free(genMmlBuf);
        genMmlBuf = nullptr;
    }
    genMmlBuf = mml;  // Take ownership of the buffer

    // Ensure it has the prefix for parseMML
    if (strncmp(genMmlBuf, "MML@", 4) != 0) {
        Serial.println("[GPT] Generated MML missing prefix");
        return;
    }

    // Count tracks
    uint8_t tc = countMMLTracks(genMmlBuf);
    Serial.printf("[GPT] Playing generated melody (%d tracks)\n", tc);

    // Parse into temporary song entry
    SongEntry& genSong = songs[genIdx];
    genSong.parsed = false;
    genSong.fmt = FMT_MML;
    genSong.trackCount = tc;
    genSong.name = "Generated Melody";
    genSong.progmemStr = nullptr;  // Not in PROGMEM
    for (uint8_t t = 0; t < MAX_TRACKS; t++) {
        genSong.tracks[t] = { nullptr, 0 };
    }

    // Parse each track using existing MML parser (it expects a RAM string)
    uint16_t (*tempBuf)[2] = (uint16_t(*)[2])malloc(MAX_NOTES_PER_SONG * sizeof(uint16_t[2]));
    if (!tempBuf) {
        Serial.println("[GPT] tempBuf malloc failed");
        return;
    }

    uint8_t tracksToParse = tc < MAX_TRACKS ? tc : MAX_TRACKS;
    for (uint8_t t = 0; t < tracksToParse; t++) {
        uint16_t count = parseMML(genMmlBuf, tempBuf, MAX_NOTES_PER_SONG, t);
        Serial.printf("[GPT] Track %d: %d notes\n", t, count);
        if (count > 0) {
            uint16_t (*notes)[2] = (uint16_t(*)[2])malloc(count * sizeof(uint16_t[2]));
            if (notes) {
                memcpy(notes, tempBuf, count * sizeof(uint16_t[2]));
                genSong.tracks[t] = { notes, count };
            }
        }
    }
    free(tempBuf);

    genSong.parsed = true;
    currentSongIndex = genIdx;
    assignTracks(genSong);
    enterState(PLAYING);
}

void genTask(void* param) {
    // PSRAM check
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 512 * 1024) {
        queueWsMessage("gen:err:low memory");
        generating = false;
        vTaskDelete(NULL);
        return;
    }

    queueWsMessage("gen:start");
    char* mml = gpt_generate(&gptModel, "MML@", 900, genTemperature,
                              streamCallback, nullptr);

    if (mml && !genAbort) {
        // Send full result
        size_t len = strlen(mml);
        char* msg = (char*)malloc(len + 16);
        if (msg) {
            snprintf(msg, len + 16, "gen:done:%s", mml);
            queueWsMessage(msg);
            free(msg);
        }
        // Queue for playback (transfer ownership of mml to main loop)
        if (xQueueSend(genResultQueue, &mml, 0) != pdTRUE) {
            free(mml);  // Queue full, free it
        }
    } else {
        if (mml) free(mml);
        queueWsMessage(genAbort ? "gen:err:aborted" : "gen:err:failed");
    }

    generating = false;
    vTaskDelete(NULL);
}

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
.vol-row{display:flex;align-items:center;gap:10px;padding:4px 20px 12px}
.vol-row span{font-size:0.7rem;color:var(--dim)}
.vol-row .vol-val{min-width:28px;text-align:right;font-variant-numeric:tabular-nums}
.vol-row input[type=range]{flex:1;height:4px;-webkit-appearance:none;appearance:none;
background:var(--border);border-radius:2px;outline:none}
.vol-row input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:18px;height:18px;
border-radius:50%;background:var(--accent);cursor:pointer}
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
.gen-link{display:none;padding:8px 20px 12px}
.gen-link a{display:block;padding:12px;border-radius:8px;background:var(--accent);color:#fff;
font-size:0.9rem;font-weight:600;text-align:center;text-decoration:none;transition:opacity .15s}
.gen-link a:active{opacity:.8}
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
<div class="vol-row">
<span>Vol</span>
<input type="range" id="vol" min="0" max="100" value="20">
<span class="vol-val" id="volVal">20%</span>
</div>
<div class="gen-link" id="genLink">
<a href="/generate">Generate Melody</a>
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
var volSlider=document.getElementById('vol');
var volVal=document.getElementById('volVal');
var genLink=document.getElementById('genLink');
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
    } else if(e.data.startsWith('vol:')){
      var v=parseInt(e.data.substring(4),10);
      volSlider.value=v;
      volVal.textContent=v+'%';
    } else if(e.data==='status:gpt:1'){
      genLink.style.display='block';
    } else if(e.data.startsWith('gen:done:')){
      now.textContent='Now Playing: Generated Melody';
      now.className='now-playing active';
      setBuzzers(true);
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
volSlider.addEventListener('input',function(){
  var v=volSlider.value;
  volVal.textContent=v+'%';
  if(sock&&sock.readyState===1)sock.send('vol:'+v);
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

static const char GENERATE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="theme-color" content="#0f0f0f">
<title>Generate Melody</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#0f0f0f;--card:#1a1a1a;--border:#2a2a2a;--text:#e0e0e0;--dim:#666;
--accent:#6c63ff;--accent2:#4ecdc4;--danger:#e53e3e;--success:#38a169}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--text);
-webkit-user-select:none;user-select:none;padding:16px 20px}
.back{display:inline-block;color:var(--accent);text-decoration:none;font-size:0.85rem;margin-bottom:16px}
.back:active{opacity:.7}
h1{font-size:1.1rem;font-weight:600;margin-bottom:16px}
.dot{width:8px;height:8px;border-radius:50%;background:var(--danger);display:inline-block;
vertical-align:middle;margin-left:8px;transition:background .3s}
.dot.ok{background:var(--success)}
.controls{margin-bottom:16px}
.row{display:flex;gap:8px;margin-bottom:12px}
.gen-btn{flex:1;padding:14px;border:none;border-radius:8px;background:var(--accent);color:#fff;
font-size:1rem;font-weight:600;cursor:pointer;transition:opacity .15s}
.gen-btn:disabled{opacity:.4;cursor:default}
.gen-btn:active:not(:disabled){opacity:.8}
.cancel-btn{padding:14px 20px;border:none;border-radius:8px;background:var(--danger);color:#fff;
font-size:1rem;font-weight:600;cursor:pointer;display:none}
.slider-row{display:flex;align-items:center;gap:10px}
.slider-row span{font-size:0.8rem;color:var(--dim)}
.slider-row input[type=range]{flex:1;height:4px;-webkit-appearance:none;appearance:none;
background:var(--border);border-radius:2px;outline:none}
.slider-row input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;
border-radius:50%;background:var(--accent2);cursor:pointer}
.val{min-width:28px;text-align:right;font-variant-numeric:tabular-nums}
.output{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:16px;
font-family:monospace;font-size:0.8rem;color:var(--accent2);min-height:120px;max-height:50vh;
overflow-y:auto;white-space:pre-wrap;word-break:break-all;display:none;margin-bottom:16px}
.status{font-size:0.8rem;color:var(--dim);text-align:center}
</style>
</head>
<body>
<a class="back" href="/">&larr; Back to Songs</a>
<h1>Generate Melody<span class="dot" id="dot"></span></h1>
<div class="controls">
<div class="row">
<button class="gen-btn" id="genBtn">Generate</button>
<button class="cancel-btn" id="cancelBtn">Cancel</button>
</div>
<div class="slider-row">
<span>Temperature</span>
<input type="range" id="temp" min="1" max="15" value="8" step="1">
<span class="val" id="tempVal">0.8</span>
</div>
</div>
<div class="output" id="output"></div>
<div class="status" id="status"></div>
<script>
var sock=null,connected=false,rTimer=null;
var dot=document.getElementById('dot');
var genBtn=document.getElementById('genBtn');
var cancelBtn=document.getElementById('cancelBtn');
var output=document.getElementById('output');
var temp=document.getElementById('temp');
var tempVal=document.getElementById('tempVal');
var status=document.getElementById('status');
var SERVER=window.location.hostname;

function reconnect(){if(!rTimer)rTimer=setTimeout(function(){rTimer=null;connect();},3000);}
function connect(){
  if(sock){sock.onopen=sock.onclose=sock.onerror=sock.onmessage=null;try{sock.close();}catch(e){}}
  try{sock=new WebSocket('ws://'+SERVER+'/ws');}catch(e){reconnect();return;}
  sock.onopen=function(){connected=true;dot.className='dot ok';};
  sock.onclose=function(){connected=false;dot.className='dot';reconnect();};
  sock.onerror=function(){connected=false;dot.className='dot';reconnect();};
  sock.onmessage=function(e){
    if(e.data==='gen:start'){
      genBtn.disabled=true;genBtn.textContent='Generating...';
      cancelBtn.style.display='';
      output.textContent='';output.style.display='block';
      status.textContent='';
    } else if(e.data.startsWith('gen:t:')){
      output.textContent+=e.data.substring(6);
      output.scrollTop=output.scrollHeight;
    } else if(e.data.startsWith('gen:done:')){
      genBtn.disabled=false;genBtn.textContent='Generate';
      cancelBtn.style.display='none';
      status.textContent='Now playing generated melody';
    } else if(e.data.startsWith('gen:err:')){
      genBtn.disabled=false;genBtn.textContent='Generate';
      cancelBtn.style.display='none';
      var err=e.data.substring(8);
      if(err!=='aborted') status.textContent='Error: '+err;
      else status.textContent='Generation cancelled';
    } else if(e.data.startsWith('playing:')){
      status.textContent='Now Playing: '+e.data.substring(8);
    } else if(e.data==='stopped'){
      status.textContent='';
    }
  };
}
genBtn.addEventListener('click',function(){
  if(sock&&sock.readyState===1)sock.send('gen');
});
cancelBtn.addEventListener('click',function(){
  if(sock&&sock.readyState===1)sock.send('gen:stop');
});
temp.addEventListener('input',function(){
  var v=(temp.value/10).toFixed(1);
  tempVal.textContent=v;
  if(sock&&sock.readyState===1)sock.send('gen:temp:'+v);
});
document.addEventListener('visibilitychange',function(){
  if(!document.hidden&&(!sock||sock.readyState!==1)){connected=false;dot.className='dot';reconnect();}
});
connect();
</script>
</body>
</html>
)rawliteral";

// ---------- state management ----------
void enterState(State s);

void startSong(uint16_t index) {
    Serial.printf("[PLAY] startSong(%d) heap=%u\n", index, ESP.getFreeHeap());
    if (currentSongIndex >= 0 && currentSongIndex < SONG_COUNT) {
        freeSongTracks(songs[currentSongIndex]);
        Serial.printf("[PLAY] Freed previous song #%d, heap=%u\n", currentSongIndex, ESP.getFreeHeap());
    }

    if (!parseSongTracks(index)) {
        Serial.printf("[PLAY] Failed to parse song #%d\n", index);
        return;
    }

    currentSongIndex = index;
    assignTracks(songs[index]);

    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        Serial.printf("[PLAY] Buzzer %d: len=%d shift=%d playing=%d\n",
            i, players[i].length, players[i].octaveShift, players[i].playing);
    }
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
        {
            char volMsg[8];
            snprintf(volMsg, sizeof(volMsg), "vol:%d", volumePercent);
            client->text(volMsg);
        }
        // Send GPT model status
        client->text(gptLoaded ? "status:gpt:1" : "status:gpt:0");
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
                        stateEnteredAt = millis(); // reset settle timer before transition to prevent cross-core race
                        stopAllBuzzers();
                    }
                    startSong(idx);
                    enterState(PLAYING);
                }
            } else if (len >= 5 && len <= 7 && memcmp(data, "vol:", 4) == 0) {
                char numBuf[4];
                memcpy(numBuf, data + 4, len - 4);
                numBuf[len - 4] = '\0';
                int v = atoi(numBuf);
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                volumePercent = v;
                char volMsg[8];
                snprintf(volMsg, sizeof(volMsg), "vol:%d", volumePercent);
                ws.textAll(volMsg);
            } else if (len == 3 && memcmp(data, "gen", 3) == 0) {
                if (!gptLoaded) {
                    client->text("gen:err:no model");
                } else if (generating) {
                    client->text("gen:err:busy");
                } else {
                    generating = true;
                    genAbort = false;
                    xTaskCreatePinnedToCore(genTask, "gpt_gen", 8192, nullptr, 1, nullptr, 0);
                }
            } else if (len >= 9 && memcmp(data, "gen:temp:", 9) == 0) {
                char tbuf[8];
                size_t tlen = len - 9;
                if (tlen > 7) tlen = 7;
                memcpy(tbuf, data + 9, tlen);
                tbuf[tlen] = '\0';
                float t = atof(tbuf);
                if (t >= 0.1f && t <= 1.5f) {
                    genTemperature = t;
                    Serial.printf("[GPT] Temperature set to %.2f\n", genTemperature);
                }
            } else if (len == 8 && memcmp(data, "gen:stop", 8) == 0) {
                genAbort = true;
                Serial.println("[GPT] Generation abort requested");
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
    Serial.printf("[BOOT] HEAP at start: %u bytes\n", ESP.getFreeHeap());

    // GPIO init for buzzers — start in INPUT mode (high-impedance) for silence
    // Pins switched to OUTPUT only during playback to avoid idle noise
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        pinMode(buzzerPins[i], INPUT);
    }

    // Timer ISR for software PWM — 40kHz sample rate
    // Timer 0, prescaler 2 (80MHz/2=40MHz), count up
    // Timer starts DISABLED - enabled only during playback to avoid idle noise
    audioTimer = timerBegin(0, TIMER_DIVIDER, true);
    timerAttachInterrupt(audioTimer, audioISR, true);
    timerAlarmWrite(audioTimer, 1000, true);  // 40MHz/1000 = 40kHz, auto-reload
    // Note: timerAlarmEnable() called in assignTracks() when playback starts
    Serial.println("[BOOT] Timer ISR init done (40kHz software PWM, idle until play)");

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

    // GPT model loading (graceful — firmware works without it)
    if (!LittleFS.begin(true)) {
        Serial.println("[GPT] LittleFS mount failed");
    } else {
        gptLoaded = gpt_load(&gptModel, "/model.bin");
        if (gptLoaded) {
            Serial.printf("[GPT] Model loaded! heap=%u, psram=%u\n",
                ESP.getFreeHeap(), ESP.getFreePsram());
        } else {
            Serial.println("[GPT] Model not found or failed — continuing without GPT");
        }
    }
    genResultQueue = xQueueCreate(1, sizeof(char*));
    wsMessageQueue = xQueueCreate(32, sizeof(char*));

    // WebSocket
    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    Serial.println("[BOOT] WebSocket handler registered");

    // HTTP routes
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(200, "text/html", INDEX_HTML);
        response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        request->send(response);
    });
    server.on("/generate", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(200, "text/html", GENERATE_HTML);
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
            if (tc > MAX_TRACKS) tc = MAX_TRACKS;
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

    // Drain WebSocket message queue (thread-safe relay from core 0 genTask)
    {
        char* wsMsg = nullptr;
        while (xQueueReceive(wsMessageQueue, &wsMsg, 0) == pdTRUE && wsMsg) {
            ws.textAll(wsMsg);
            free(wsMsg);
        }
    }

    // Check for generated melody to play
    {
        char* genMml = nullptr;
        if (xQueueReceive(genResultQueue, &genMml, 0) == pdTRUE && genMml) {
            if (state == PLAYING) {
                stopAllBuzzers();
            }
            playGeneratedMML(genMml);
            // genMml ownership transferred to playGeneratedMML
        }
    }

    // Update all players
    for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
        updatePlayer(players[i]);
    }

    // Periodic playback status (every 2s)
    if (state == PLAYING) {
        static unsigned long lastStatusAt = 0;
        unsigned long now = millis();
        if (now - lastStatusAt >= 2000) {
            lastStatusAt = now;
            Serial.printf("[STATUS] t=%lus | ", now / 1000);
            for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
                if (players[i].playing) {
                    Serial.printf("B%d:%d/%d ", i, players[i].noteIndex, players[i].length);
                }
            }
            Serial.println();
        }
    }

    // Synchronized looping: when all active players finish, restart together
    if (state == PLAYING && anyPlayerActive() && allPlayersInLoopPause()) {
        Serial.println("[LOOP] All tracks finished — restarting");
        unsigned long startTime = millis();
        for (uint8_t i = 0; i < NUM_BUZZERS; i++) {
            MelodyPlayer& p = players[i];
            if (p.playing) {
                p.noteIndex = 0;
                p.inLoopPause = false;
                p.noteStartedAt = startTime;
                setupNote(p);
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
