#include "csurf.h"
#ifdef _WIN32
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#else
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#include "dm2000_compat.h"

// DM2000 fader taper calibrated via DM2000 Editor after running the console's built-in
// fader calibration utility (MIDI-OX capture 2026-06-15; doc/fader-calibration-2026-06-15.txt).
// 14-bit wire value = (MSB<<7)|LSB. Physical maximum = 16383 at the printed +10 mark.
// Taper is linear from -30 to +10 dB (1600 wire units per 5 dB); compressed below -30.
static const double g_taper_db[]  = { -150.0, -50.0, -40.0, -30.0, -20.0, -15.0, -10.0,  -5.0,   0.0,  5.0, 10.0 };
static const int    g_taper_val[] = {      0,  2768,  3568,  5168,  6768,  8368,  9968, 11568, 13168, 14768, 16383 };
#define TAPER_N (sizeof(g_taper_val) / sizeof(g_taper_val[0]))

static double int14ToVol(unsigned char msb, unsigned char lsb)
{
    int val = lsb | (msb << 7);
    if (val <= g_taper_val[0]) return 0.0; // bottom = -inf
    if (val >= g_taper_val[TAPER_N - 1]) return DB2VAL(g_taper_db[TAPER_N - 1]);

    unsigned int i = 1;
    while (i < TAPER_N - 1 && val > g_taper_val[i]) i++;
    double db = g_taper_db[i - 1] + (g_taper_db[i] - g_taper_db[i - 1]) *
        (double)(val - g_taper_val[i - 1]) / (double)(g_taper_val[i] - g_taper_val[i - 1]);
    return DB2VAL(db);
}

static int volToInt14(double vol)
{
    double db = VAL2DB(vol);
    if (db <= g_taper_db[0]) return g_taper_val[0];
    if (db >= g_taper_db[TAPER_N - 1]) return g_taper_val[TAPER_N - 1];

    unsigned int i = 1;
    while (i < TAPER_N - 1 && db > g_taper_db[i]) i++;
    double v = g_taper_val[i - 1] + (g_taper_val[i] - g_taper_val[i - 1]) *
        (db - g_taper_db[i - 1]) / (g_taper_db[i] - g_taper_db[i - 1]);
    return (int)(v + 0.5);
}

static unsigned char peakToMeter(double pk)
{
    // the DM2000's red OVER segment IS level 0x0C -- it ignores the 0x0E "clip"
    // code from the HUI docs (hardware-verified: 0x0E never lit the red LED).
    // Send 0x0C only strictly above 0 dBFS so red matches REAPER's indication.
    if (pk > 1.0) return 0x0C;

    // map -60..0 dB onto segments 0x00..0x0B, keeping 0x0C exclusive to clip
    int l = (int)((VAL2DB(pk) + 60.0) * (12.0 / 60.0) + 0.5);
    if (l < 0) l = 0;
    else if (l > 0x0B) l = 0x0B;
    return (unsigned char)l;
}

static unsigned char panToChar(double pan)
{
    pan = (pan + 1.0) * 63.5;

    if (pan < 0.0) pan = 0.0;
    else if (pan > 127.0) pan = 127.0;

    return (unsigned char)(pan + 0.5);
}

static void GetIniPath(char *buf, int bufsz); // defined below; used in constructor

class CSurf_DM2000 : public IReaperControlSurface
{
    int m_midi_in_devs[4];
    int m_midi_out_devs[4];
    midi_Output *m_midiouts[4];
    midi_Input *m_midiins[4];
    midi_Output *m_midiout8;         // DM2000 native SysEx output (port 8); reserved for scene recall

    int m_zone[4];                   // last switch-matrix zone select (B0 0F zz) per port
    unsigned char m_fader_msb[4][8]; // pending fader value MSB per port/channel
    char m_fader_touch[32];
    int m_vol_lastpos[32];
    int m_pan_lastpos[32];
    unsigned int m_pan_lasttouch[32];
    unsigned char m_meter_lastlvl[32];
    unsigned char m_meter_hist[32][2][3]; // per-channel/side level history: peak hold over 3 polls
    int m_meter_histpos;
    DWORD m_meter_lastrun;
    int m_bank_offset;
    int m_wheel_mode;                // 0=jog (edit cursor), 1=scrub, 2=shuttle (coarse)
    int m_arrow_mode;                // ENTER cycles cursor arrows: 0=scroll, 1=zoom, 2=bank-scroll
    int m_auto_mode;                 // 0=trim/bypass, 1=read, 2=touch, 3=write, 4=latch, 5=latch preview
    char m_ini_path[MAX_PATH];       // user-configured path to dm2000_keys.ini (empty = default)
    int m_held_arrow;                // -1=none, 0-3=CSurf_OnArrow direction being held
    DWORD m_arrow_held_since;        // timestamp of the press that started the hold
    DWORD m_arrow_last_repeat;       // timestamp of the last auto-repeat fire
    int m_held_transport;            // -1=none, 1=REW, 2=FF
    DWORD m_transport_held_since;
    DWORD m_transport_last_repeat;
    unsigned char m_tc_lastbuf[8];   // encoded HUI counter bytes for change detection
    // Locate section: configurable action IDs loaded from [locate] in dm2000_keys.ini.
    // RTZ/END: 0 = use CSurf_GoStart()/CSurf_GoEnd() API; non-zero = dispatch that action ID.
    // All others: 0 = no action; non-zero = dispatch that action ID.
    int m_la_rtz, m_la_end, m_la_loop, m_la_qpunch; // zone 0x0F hw-verified buttons
    int m_la_in, m_la_out, m_la_post;                // zone 0x10 hw-verified buttons
    int m_la_lm[8];                                  // LM1-LM8 locate memory buttons

public:
  CSurf_DM2000(int indev1, int outdev1, int indev2, int outdev2, int indev3, int outdev3, int indev4, int outdev4, const char *iniPath, int *errStats)
  {
    m_midi_in_devs[0] = indev1;
    m_midi_out_devs[0] = outdev1;
    m_midi_in_devs[1] = indev2;
    m_midi_out_devs[1] = outdev2;
    m_midi_in_devs[2] = indev3;
    m_midi_out_devs[2] = outdev3;
    m_midi_in_devs[3] = indev4;
    m_midi_out_devs[3] = outdev4;

    m_bank_offset = 0;
    m_zone[0] = m_zone[1] = m_zone[2] = m_zone[3] = -1;
    memset(m_fader_msb, 0, sizeof(m_fader_msb));
    memset(m_fader_touch, 0, sizeof(m_fader_touch));
    memset(m_vol_lastpos, 0xff, sizeof(m_vol_lastpos));
    memset(m_pan_lastpos, 0xff, sizeof(m_pan_lastpos));
    memset(m_pan_lasttouch, 0, sizeof(m_pan_lasttouch));
    memset(m_meter_lastlvl, 0xff, sizeof(m_meter_lastlvl));
    memset(m_meter_hist, 0, sizeof(m_meter_hist));
    m_meter_histpos = 0;
    m_meter_lastrun = 0;
    m_wheel_mode = 0;
    m_arrow_mode = 0;
    m_auto_mode  = 1;
    sprintf_s(m_ini_path, sizeof(m_ini_path), "%s", iniPath ? iniPath : "");
    m_held_arrow = -1;
    m_arrow_held_since = 0;
    m_arrow_last_repeat = 0;
    m_held_transport = -1;
    m_transport_held_since = 0;
    m_transport_last_repeat = 0;
    memset(m_tc_lastbuf, 0xFF, sizeof(m_tc_lastbuf));

    // locate section: all buttons default to no-op; configure via [locate] in dm2000_keys.ini.
    // RTZ/END: 0 = CSurf_GoStart/GoEnd API (fundamental transport, always works).
    // All other buttons: 0 = no action until ini is configured.
    m_la_rtz     = 0;
    m_la_end     = 0;
    m_la_loop    = 0;
    m_la_qpunch  = 0;
    m_la_in      = 0;
    m_la_out     = 0;
    m_la_post    = 0;
    for (int i = 0; i < 8; i++)
        m_la_lm[i] = 0;

    // load [locate] overrides from ini
    char _la_ini[MAX_PATH];
    if (m_ini_path[0])
        sprintf_s(_la_ini, sizeof(_la_ini), "%s", m_ini_path);
    else
        GetIniPath(_la_ini, sizeof(_la_ini));
    if (_la_ini[0])
    {
        m_la_rtz    = GetPrivateProfileInt("locate", "rtz",    m_la_rtz,    _la_ini);
        m_la_end    = GetPrivateProfileInt("locate", "end",    m_la_end,    _la_ini);
        m_la_loop   = GetPrivateProfileInt("locate", "loop",   m_la_loop,   _la_ini);
        m_la_qpunch = GetPrivateProfileInt("locate", "qpunch", m_la_qpunch, _la_ini);
        m_la_in     = GetPrivateProfileInt("locate", "in",     m_la_in,     _la_ini);
        m_la_out    = GetPrivateProfileInt("locate", "out",    m_la_out,    _la_ini);
        m_la_post   = GetPrivateProfileInt("locate", "post",   m_la_post,   _la_ini);
        for (int i = 0; i < 8; i++)
        {
            char key[8];
            sprintf_s(key, sizeof(key), "lm%d", i + 1);
            m_la_lm[i] = GetPrivateProfileInt("locate", key, m_la_lm[i], _la_ini);
        }
    }

    for (int i = 0; i < 4; ++i)
    {
        m_midiins[i] = m_midi_in_devs[i] >= 0 ? CreateMIDIInput(m_midi_in_devs[i]) : NULL;
        m_midiouts[i] = m_midi_out_devs[i] >= 0 ? CreateThreadedMIDIOutput(CreateMIDIOutput(m_midi_out_devs[i], false, NULL)) : NULL;

        if (errStats)
        {
            if (m_midi_in_devs[i] >= 0 && !m_midiins[i]) *errStats |= 1;
            if (m_midi_out_devs[i] >= 0 && !m_midiouts[i]) *errStats |= 2;
        }

        if (m_midiins[i])
            m_midiins[i]->start();
    }

    m_midiout8 = NULL;
    SendCounter(true); // blank counter display and sync m_tc_lastbuf to known state
  }

  ~CSurf_DM2000()
  {
      CloseNoReset();
  }

  const char *GetTypeString() { return "DM2000"; }
  const char *GetDescString() { return "Yamaha DM2000"; }
  const char *GetConfigString() // string of configuration data
  {
    static char configtmp[512];
    if (m_ini_path[0])
        sprintf(configtmp, "%d %d %d %d %d %d %d %d|%s", m_midi_in_devs[0], m_midi_out_devs[0], m_midi_in_devs[1], m_midi_out_devs[1], m_midi_in_devs[2], m_midi_out_devs[2], m_midi_in_devs[3], m_midi_out_devs[3], m_ini_path);
    else
        sprintf(configtmp, "%d %d %d %d %d %d %d %d", m_midi_in_devs[0], m_midi_out_devs[0], m_midi_in_devs[1], m_midi_out_devs[1], m_midi_in_devs[2], m_midi_out_devs[2], m_midi_in_devs[3], m_midi_out_devs[3]);
    return configtmp;
  }

  void CloseNoReset()
  {
      // HUI has no "goodbye" message -- the console flags DAW Off-line by itself
      // once the ping echo stops -- but clear our state so the remote layer does
      // not keep showing a frozen mix: meters off, LEDs off, pan rings cleared,
      // scribbles blanked, faders driven to -inf.
      for (int i = 0; i < 32; ++i)
      {
          if (m_midiouts[i / 8])
          {
              int ch = i & 7;
              m_midiouts[i / 8]->Send(0xB0, ch, 0, -1);             // fader MSB = 0 (-inf)
              m_midiouts[i / 8]->Send(0xB0, 0x20 + ch, 0, -1);      // fader LSB = 0
              m_midiouts[i / 8]->Send(0xA0, i & 7, 0x00, -1);       // meter L off
              m_midiouts[i / 8]->Send(0xA0, i & 7, 0x10, -1);       // meter R off
              m_midiouts[i / 8]->Send(0xB0, 0x10 + (i & 7), 0, -1); // pan ring off
          }
          SendChannelLED(i, 1, false);
          SendChannelLED(i, 2, false);
          SendChannelLED(i, 3, false);
          SendChannelLED(i, 7, false);
          SendTrackTitle(i, "");
      }
      SendTransportLED(3, false);
      SendTransportLED(4, false);
      SendTransportLED(5, false);
      SendCounter(true); // blank the LED counter display

      // REAPER's MIDI outputs queue internally (CreateThreadedMIDIOutput is a
      // passthrough on Windows); deleting them immediately drops whatever the
      // driver hasn't drained yet, leaving stale state on the console
      Sleep(200);

      for (int i = 0; i < 4; ++i)
      {
          delete m_midiouts[i];
          delete m_midiins[i];
          m_midiouts[i] = nullptr;
          m_midiins[i] = nullptr;
      }
      delete m_midiout8;
      m_midiout8 = nullptr;
  }

  void Run()
  {
      for (int i = 0; i < 4; ++i)
      {
          if (m_midiins[i])
          {
              m_midiins[i]->SwapBufs(timeGetTime());
              int l = 0;
              MIDI_eventlist *list = m_midiins[i]->GetReadBuf();
              MIDI_event_t *evts;
              while ((evts = list->EnumItems(&l))) OnMIDIEvent(evts, i);
          }
      }

      DWORD now = timeGetTime();

      // arrow auto-repeat: fire after 400ms hold, then every 80ms
      if (m_held_arrow >= 0 && now - m_arrow_held_since > 400)
      {
          if (now - m_arrow_last_repeat > 80)
          {
              if (m_arrow_mode == 2 && (m_held_arrow == 2 || m_held_arrow == 3))
              {
                  int dir = m_held_arrow;
                  AdjustBankOffset(dir == 2 ? -1 : 1);
                  m_held_arrow = dir;
                  if (SetMixerScroll)
                  {
                      MediaTrack *t = CSurf_TrackFromID(m_bank_offset + 1, false);
                      if (t) SetMixerScroll(t);
                  }
              }
              else
              {
                  CSurf_OnArrow(m_held_arrow, m_arrow_mode == 1);
              }
              m_arrow_last_repeat = now;
          }
      }

      // REW/FF auto-repeat: same timing as arrows
      if (m_held_transport >= 0 && now - m_transport_held_since > 400)
      {
          if (now - m_transport_last_repeat > 80)
          {
              if (m_held_transport == 1) CSurf_OnRew(1);
              else                       CSurf_OnFwd(1);
              m_transport_last_repeat = now;
          }
      }

      if (now - m_meter_lastrun >= 100) // unsigned diff also handles timer wrap
      {
          m_meter_lastrun = now;
          SendCounter();
          m_meter_histpos = (m_meter_histpos + 1) % 3;
          for (int i = 0; i < 32; ++i)
          {
              if (!m_midiouts[i / 8]) continue;

              // trackless channels fall through with level 0 so their meter clears
              unsigned char lvlL = 0, lvlR = 0;
              MediaTrack *tr = TrackFromCh(i);
              if (tr)
              {
                  lvlL = peakToMeter(Track_GetPeakInfo(tr, 0));
                  lvlR = peakToMeter(Track_GetPeakInfo(tr, 1));
              }

              // peak hold: send the max of the last 3 polls to steady the display
              m_meter_hist[i][0][m_meter_histpos] = lvlL;
              m_meter_hist[i][1][m_meter_histpos] = lvlR;
              for (int h = 0; h < 3; ++h)
              {
                  if (m_meter_hist[i][0][h] > lvlL) lvlL = m_meter_hist[i][0][h];
                  if (m_meter_hist[i][1][h] > lvlR) lvlR = m_meter_hist[i][1][h];
              }

              unsigned char packed = (unsigned char)((lvlL << 4) | lvlR);
              if (m_meter_lastlvl[i] != packed)
              {
                  m_meter_lastlvl[i] = packed;
                  // HUI meters: poly key pressure, value = (side << 4) | level
                  m_midiouts[i / 8]->Send(0xA0, i & 7, lvlL, -1);        // left
                  m_midiouts[i / 8]->Send(0xA0, i & 7, 0x10 | lvlR, -1); // right
              }
          }
      }
  }

  void SetTrackListChange()
  {
      // blank scribble strips on surface channels that no longer map to a track
      for (int i = 0; i < 32; ++i)
          if (!TrackFromCh(i)) SendTrackTitle(i, "");
  }
  void SetSurfaceVolume(MediaTrack *trackid, double volume)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id < 0 || id >= 32 || !m_midiouts[id / 8]) return;

      int volint = volToInt14(volume);
      if (m_vol_lastpos[id] != volint)
      {
          m_vol_lastpos[id] = volint;
          int ch = id & 7;
          m_midiouts[id / 8]->Send(0xB0, ch, (volint >> 7) & 0x7F, -1);
          m_midiouts[id / 8]->Send(0xB0, 0x20 + ch, volint & 0x7F, -1);
      }
  }
  void SetSurfacePan(MediaTrack *trackid, double pan)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id < 0 || id >= 32 || !m_midiouts[id / 8]) return;

      unsigned char panch = panToChar(pan);
      if (m_pan_lastpos[id] != panch)
      {
          m_pan_lastpos[id] = panch;
          m_midiouts[id / 8]->Send(0xB0, 0x10 + (id & 7), 1 + ((panch * 11) >> 7), -1); // ring LED 1-11
      }
  }
  void SetSurfaceMute(MediaTrack *trackid, bool mute)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id >= 0 && id < 32) SendChannelLED(id, 2, mute);
  }
  void SetSurfaceSelected(MediaTrack *trackid, bool selected)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id >= 0 && id < 32) SendChannelLED(id, 1, selected);
  }
  void SetSurfaceSolo(MediaTrack *trackid, bool solo)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id >= 0 && id < 32) SendChannelLED(id, 3, solo);
  }
  void SetSurfaceRecArm(MediaTrack *trackid, bool recarm)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id >= 0 && id < 32) SendChannelLED(id, 7, recarm);
  }
  void SetPlayState(bool play, bool pause, bool rec)
  {
      SendTransportLED(3, !play && !pause); // STOP
      SendTransportLED(4, play);            // PLAY
      SendTransportLED(5, rec);             // REC
  }
  void SetRepeatState(bool rep) { SendGlobalLED(0x0F, 3, rep); } // LOOP is zone 0x0F sw3 (hw-verified 2026-06-15)

  void SetTrackTitle(MediaTrack *trackid, const char *title)
  {
      SendTrackTitle(CSurf_TrackToID(trackid, false) - 1 - m_bank_offset, title);
  }
  bool GetTouchState(MediaTrack *trackid, int isPan)
  {
      int id = CSurf_TrackToID(trackid, false) - 1 - m_bank_offset;
      if (id < 0 || id >= 32) return false;
      if (isPan == 1)
      {
          DWORD now = timeGetTime();
          // fake touch, go for 3s after last knob movement
          return now < m_pan_lasttouch[id] + 3000 && now >= m_pan_lasttouch[id] - 1000;
      }
      return !!m_fader_touch[id];
  }
  void SetAutoMode(int mode)
  {
      m_auto_mode = mode;
      SendAutomixLEDs(mode);
  }
  void ResetCachedVolPanStates()
  {
      memset(m_vol_lastpos, 0xff, sizeof(m_vol_lastpos));
      memset(m_pan_lastpos, 0xff, sizeof(m_pan_lastpos));
  }
  void OnTrackSelection(MediaTrack *trackid) {}

  bool IsKeyDown(int key) { return false; }

private:
    // surface channel (0..23) -> REAPER track, skipping master (track id 0)
    MediaTrack *TrackFromCh(int gch)
    {
        return CSurf_TrackFromID(m_bank_offset + gch + 1, false);
    }

    // HUI channel scribble strip, same byte format as csurf_babyhui.cpp:
    // F0 00 00 66 05 00 10 <ch> <4 chars> F7
    void SendTrackTitle(int id, const char *title)
    {
        if (id < 0 || id >= 32 || !m_midiouts[id / 8]) return;

        int len = title ? (int)strlen(title) : 0;

        unsigned char sysex[13] = { 0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7 };
        sysex[7] = id & 7;
        for (int i = 0; i < 4 && i < len; ++i)
            sysex[8 + i] = title[i] & 0x7F;

        char buf[sizeof(MIDI_event_t) + sizeof(sysex)];
        MIDI_event_t *msg = (MIDI_event_t *)buf;
        msg->frame_offset = -1;
        msg->size = sizeof(sysex);
        memcpy(msg->midi_message, sysex, sizeof(sysex));
        m_midiouts[id / 8]->SendMsg(msg, -1);

        // Layer 3: native channel name on port 8, hardware-captured 2026-06-12.
        // Studio Manager sends: F0 43 17 3E 06 02 04 [pos] [ch] 00 00 00 [ASCII] F7
        // Studio Manager used pos=0..3; we send 0..7 to probe 8-char display support.
        if (m_midiout8)
        {
            for (int pos = 0; pos < 8; ++pos)
            {
                unsigned char c = (pos < len) ? (title[pos] & 0x7F) : 0x20;
                unsigned char buf[] = { 0xF0, 0x43, 0x17, 0x3E, 0x06, 0x02, 0x04,
                                        (unsigned char)pos, (unsigned char)(id & 0x7F),
                                        0x00, 0x00, 0x00, c, 0xF7 };
                SendSysEx(buf, sizeof(buf));
            }
        }
    }

    // HUI counter (LED timecode) display.
    // Protocol captured 2026-06-15 from Pro Tools via loopMIDI on port 1:
    //   F0 00 00 66 05 00 11 [N bytes right-to-left] F7
    // Each byte: high nibble = 1 if a separator (. or :) follows that digit in the
    // display; low nibble = BCD digit 0-9. Only positions that changed since the
    // previous message are sent, spanning from the rightmost to leftmost change.
    // Special clear: 8 x 0x20 blanks all positions (used at init and on close).
    void SendCounter(bool clear = false)
    {
        if (!m_midiouts[0]) return;

        if (clear)
        {
            unsigned char blank[17] = {
                0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x11,
                0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
                0xF7
            };
            char msgbuf[sizeof(MIDI_event_t) + 17];
            MIDI_event_t *msg = (MIDI_event_t *)msgbuf;
            msg->frame_offset = -1;
            msg->size = 17;
            memcpy(msg->midi_message, blank, 17);
            m_midiouts[0]->SendMsg(msg, -1);
            memset(m_tc_lastbuf, 0x20, sizeof(m_tc_lastbuf));
            return;
        }

        // Read transport display mode: prefer timemode2 (right-click transport setting)
        // over timemode (project primary), same logic as csurf_mcu.cpp.
        int tmode = 0;
        int *tmodeptr = (int *)projectconfig_var_addr(NULL, __g_projectconfig_timemode2);
        if (tmodeptr && *tmodeptr >= 0) tmode = *tmodeptr;
        else {
            tmodeptr = (int *)projectconfig_var_addr(NULL, __g_projectconfig_timemode);
            if (tmodeptr) tmode = *tmodeptr;
        }

        double pos = (GetPlayState() & 1) ? GetPlayPosition() : GetCursorPosition();
        char tbuf[64];
        format_timestr_pos(pos, tbuf, sizeof(tbuf), tmode);

        // Parse the time string right-to-left into 8 display positions.
        // Separator chars (: . ,) mark the digit to their left as having a separator after it.
        unsigned char disp[8];
        memset(disp, 0x20, sizeof(disp)); // 0x20 = blank position

        int slen = (int)strlen(tbuf);
        int dpos = 7;
        bool sep_follows = false;
        for (int i = slen - 1; i >= 0 && dpos >= 0; --i)
        {
            char c = tbuf[i];
            if (c >= '0' && c <= '9')
            {
                disp[dpos--] = (unsigned char)((sep_follows ? 0x10 : 0x00) | (c - '0'));
                sep_follows = false;
            }
            else if (c == '.' || c == ':' || c == ',')
            {
                sep_follows = true;
            }
        }

        // Find the leftmost and rightmost positions that changed.
        int lmc = -1, rmc = -1;
        for (int i = 0; i < 8; ++i)
        {
            if (disp[i] != m_tc_lastbuf[i])
            {
                if (lmc < 0) lmc = i;
                rmc = i;
            }
        }
        if (lmc < 0) return;

        memcpy(m_tc_lastbuf, disp, 8);

        unsigned char sysex[16]; // 7 header + up to 8 data + 1 F7
        int si = 0;
        sysex[si++] = 0xF0; sysex[si++] = 0x00; sysex[si++] = 0x00;
        sysex[si++] = 0x66; sysex[si++] = 0x05; sysex[si++] = 0x00;
        sysex[si++] = 0x11;
        for (int i = rmc; i >= lmc; --i)
            sysex[si++] = disp[i];
        sysex[si++] = 0xF7;

        char msgbuf[sizeof(MIDI_event_t) + 16];
        MIDI_event_t *msg = (MIDI_event_t *)msgbuf;
        msg->frame_offset = -1;
        msg->size = si;
        memcpy(msg->midi_message, sysex, si);
        m_midiouts[0]->SendMsg(msg, -1);
    }

    // native Yamaha SysEx out on USB port 8 (Layer 3 features: 8-char names,
    // meter bridge, scene recall); data must be a complete F0..F7 message
    void SendSysEx(const unsigned char *data, int len)
    {
        if (!m_midiout8 || len < 2 || len > 200) return;

        char buf[sizeof(MIDI_event_t) + 200];
        MIDI_event_t *msg = (MIDI_event_t *)buf;
        msg->frame_offset = -1;
        msg->size = len;
        memcpy(msg->midi_message, data, len);
        m_midiout8->SendMsg(msg, -1);
    }

    // host->surface switch LED: zone select on 0x0C, switch|state on 0x2C
    void SendChannelLED(int id, int sw, bool state)
    {
        if (id < 0 || id >= 32 || !m_midiouts[id / 8]) return;
        m_midiouts[id / 8]->Send(0xB0, 0x0C, id & 7, -1);
        m_midiouts[id / 8]->Send(0xB0, 0x2C, (state ? 0x40 : 0x00) | sw, -1);
    }

    // LED in a global (non-channel) zone; these controls live on the first HUI unit (port 1)
    void SendGlobalLED(int zone, int sw, bool state)
    {
        if (!m_midiouts[0]) return;
        m_midiouts[0]->Send(0xB0, 0x0C, zone, -1);
        m_midiouts[0]->Send(0xB0, 0x2C, (state ? 0x40 : 0x00) | sw, -1);
    }

    void SendTransportLED(int sw, bool state) { SendGlobalLED(0x0E, sw, state); }

    // light the one AUTOMIX button for the current mode
    // hw-verified: enable=z0x19sw2, return=z0x18sw1, touch=z0x18sw0, write=z0x0Csw2, latch=z0x18sw5, rel=z0x18sw2
    void SendAutomixLEDs(int mode)
    {
        int lit_z = -1, lit_sw = -1;
        switch (mode)
        {
            case 0: lit_z = 0x19; lit_sw = 2; break; // bypass -> ENABLE
            case 1: lit_z = 0x18; lit_sw = 1; break; // read -> RETURN
            case 2: lit_z = 0x18; lit_sw = 0; break; // touch -> TOUCH SENSE
            case 3: lit_z = 0x0C; lit_sw = 2; break; // write -> REC
            case 4: lit_z = 0x18; lit_sw = 5; break; // latch -> AUTO-REC
            case 5: lit_z = 0x18; lit_sw = 2; break; // latch preview -> RELATIVE
        }
        for (int p = 0; p < 3; ++p)
        {
            if (!m_midiouts[p]) continue;
            for (int s = 0; s < 8; ++s)
            {
                m_midiouts[p]->Send(0xB0, 0x0C, 0x18, -1);
                m_midiouts[p]->Send(0xB0, 0x2C, (lit_z == 0x18 && s == lit_sw ? 0x40 : 0x00) | s, -1);
            }
            m_midiouts[p]->Send(0xB0, 0x0C, 0x19, -1);
            m_midiouts[p]->Send(0xB0, 0x2C, (lit_z == 0x19 ? 0x42 : 0x02), -1); // sw2 on/off
            m_midiouts[p]->Send(0xB0, 0x0C, 0x0C, -1);
            m_midiouts[p]->Send(0xB0, 0x2C, (lit_z == 0x0C ? 0x42 : 0x02), -1); // sw2 on/off
        }
    }

    void OnMIDIEvent(MIDI_event_t *evt, int port)
    {
        unsigned char status = evt->midi_message[0] & 0xF0;
        unsigned char data1  = evt->midi_message[1];
        unsigned char data2  = evt->midi_message[2];

        // Program Change on any port: DM2000 GENERAL port scene recall.
        // User must assign GENERAL to one of the 4 DAW USB ports on the console.
        // Wire format is 0-indexed (manual p.370: "Program number 0-127"):
        //   byte 0x00 = PC 1 = Scene 1 -> marker 1
        //   byte 0x62 = PC 99 = Scene 99 -> marker 99
        //   byte 0x63 = PC 100 = Scene 0 (no marker jump)
        // UNVERIFIED: some Yamaha devices use 1-indexed bytes; test with MIDI-OX.
        if (status == 0xC0)
        {
            if (data1 <= 98)
                SendMessage(g_hwnd, WM_COMMAND, ID_GOTO_MARKER1 + data1, 0);
            return;
        }

        // HUI keepalive ping - must echo back or surface goes offline
        if (status == 0x90 && data1 == 0x00 && data2 == 0x7F)
        {
            if (m_midiouts[port])
                m_midiouts[port]->Send(0x90, 0x00, 0x7F, -1);
            return;
        }

        if (status != 0xB0) return;

        if (data1 == 0x0F)                       // switch matrix: zone select
        {
            m_zone[port] = data2;
        }
        else if (data1 == 0x2F)                  // switch matrix: switch + state in current zone
        {
            OnSwitch(port, m_zone[port], data2);
        }
        else if (data1 < 0x08)                   // fader value MSB, channel 0-7 of this port
        {
            m_fader_msb[port][data1] = data2;
        }
        else if (data1 >= 0x20 && data1 < 0x28)  // fader value LSB -> apply the move
        {
            int ch = data1 - 0x20;
            MediaTrack *tr = TrackFromCh(port * 8 + ch);
            if (tr)
                // ignoresurf=NULL on purpose: the DM2000 keeps an internal model of
                // the DAW's fader positions and springs the motor back to it on touch
                // release, so our own moves must be echoed back (Pro Tools does this)
                CSurf_SetSurfaceVolume(tr, CSurf_OnVolumeChange(tr, int14ToVol(m_fader_msb[port][ch], data2), false), NULL);
        }
        else if (data1 == 0x0D)                  // jog wheel: bits 0-5 = speed (1-6), bit 6 = direction
        {
            // Direction verified on hardware: bit 6 SET moves forward (the initial report had it reversed)
            int speed = data2 & 0x3F;
            if (speed)
            {
                double dir = (data2 & 0x40) ? 1.0 : -1.0;
                if (m_wheel_mode == 1)      CSurf_ScrubAmt(dir * speed * 0.5);        // SCRUB: audible scrub
                else if (m_wheel_mode == 2) MoveEditCursor(dir * speed * 1.0, false); // SHUTTLE: coarse, 1s/click
                else                        MoveEditCursor(dir * speed * 0.1, false); // jog: 0.1s/click
            }
        }
        else if (data1 >= 0x40 && data1 < 0x48)  // pan v-pot delta: bits 0-5 = amount, bit 6 = right
        {
            int gch = port * 8 + (data1 - 0x40);
            double adj = (data2 & 0x3F) / -63.0;
            if (data2 & 0x40) adj = -adj;

            MediaTrack *tr = TrackFromCh(gch);
            if (tr) CSurf_SetSurfacePan(tr, CSurf_OnPanChange(tr, adj, true), NULL);

            m_pan_lasttouch[gch] = timeGetTime();
        }
    }

    // val: 0x40|sw = press, 0x00|sw = release
    void OnSwitch(int port, int zone, unsigned char val)
    {
        if (zone < 0) return;
        bool press = !!(val & 0x40);
        int sw = val & 0x0F;

        if (zone < 8)                            // channel strip zones: zone = channel on this port
        {
            int gch = port * 8 + zone;
            if (sw == 0)                         // fader touch / release
            {
                m_fader_touch[gch] = press ? 1 : 0;
            }
            else if (press)
            {
                MediaTrack *tr = TrackFromCh(gch);
                if (!tr) return;
                switch (sw)
                {
                    case 1: CSurf_SetSurfaceSelected(tr, CSurf_OnSelectedChange(tr, -1), NULL); break; // SELECT
                    case 2: CSurf_SetSurfaceMute(tr, CSurf_OnMuteChange(tr, -1), NULL); break; // MUTE
                    case 3: CSurf_SetSurfaceSolo(tr, CSurf_OnSoloChange(tr, -1), NULL); break; // SOLO
                    case 4: // AUTO button -> reset fader to 0 dB (unity gain = 1.0)
                        CSurf_SetSurfaceVolume(tr, CSurf_OnVolumeChange(tr, 1.0, false), NULL);
                        break;
                    case 5: // pan knob press -> center pan
                        CSurf_SetSurfacePan(tr, CSurf_OnPanChange(tr, 0.0, false), NULL);
                        m_pan_lasttouch[gch] = timeGetTime();
                        break;
                    case 7: CSurf_OnRecArmChange(tr, -1); break;                               // REC/RDY
                }
            }
        }
        else if (zone == 0x08 && press && port == 0) // BACK=sw2 FORWARD=sw6 -> undo/redo (hw-verified 2026-06-15)
        {
            if (sw == 2)
                SendMessage(g_hwnd, WM_COMMAND, IDC_EDIT_UNDO, 0);
            else if (sw == 6)
                SendMessage(g_hwnd, WM_COMMAND, IDC_EDIT_REDO, 0);
        }
        else if (zone == 0x0A && press)          // bank/channel arrows
        {
            int amt = 0;
            switch (sw)
            {
                case 0: amt = -1; break;         // channel left
                case 1: amt = -24; break;        // bank left
                case 2: amt = 1; break;          // channel right
                case 3: amt = 24; break;         // bank right
            }
            if (amt) AdjustBankOffset(amt);
        }
        else if (zone == 0x0E && !press && (sw == 1 || sw == 2))
        {
            m_held_transport = -1;               // REW/FF released: cancel auto-repeat
        }
        else if (zone == 0x0E && press)          // transport (hw-verified: sw1=REW sw2=FF sw3=STOP sw4=PLAY sw5=REC)
        {
            switch (sw)
            {
                case 1: CSurf_OnRew(1); m_held_transport = 1; m_transport_held_since = timeGetTime(); break;
                case 2: CSurf_OnFwd(1); m_held_transport = 2; m_transport_held_since = timeGetTime(); break;
                case 3: CSurf_OnStop(); break;
                case 4: CSurf_OnPlay(); break;
                case 5: CSurf_OnRecord(); break;
            }
        }
        else if (zone == 0x0F && press)  // hw-verified 2026-06-15: sw0=RTZ sw1=END sw2=ONLINE sw3=LOOP sw4=QUICK PUNCH
        {                                // SET/REHEARSAL/MTR/MASTER do not transmit HUI - DM2000 internal only
            switch (sw)
            {
                case 0: if (m_la_rtz)    SendMessage(g_hwnd, WM_COMMAND, m_la_rtz,    0); else CSurf_GoStart(); break; // RTZ
                case 1: if (m_la_end)    SendMessage(g_hwnd, WM_COMMAND, m_la_end,    0); else CSurf_GoEnd();   break; // END
                case 3: if (m_la_loop)   SendMessage(g_hwnd, WM_COMMAND, m_la_loop,   0); break;                       // LOOP
                case 4: if (m_la_qpunch) SendMessage(g_hwnd, WM_COMMAND, m_la_qpunch, 0); break;                       // QUICK PUNCH
            }
        }
        else if (zone == 0x10 && press)  // hw-verified 2026-06-15: sw0=AUDITION sw1=PRE sw2=IN sw3=OUT sw4=POST
        {
            switch (sw)
            {
                case 2: if (m_la_in)   SendMessage(g_hwnd, WM_COMMAND, m_la_in,   0); break; // IN
                case 3: if (m_la_out)  SendMessage(g_hwnd, WM_COMMAND, m_la_out,  0); break; // OUT
                case 4: if (m_la_post) SendMessage(g_hwnd, WM_COMMAND, m_la_post, 0); break; // POST
            }
        }
        else if (zone == 0x0D && !press && (sw == 4 || sw == 0 || sw == 1 || sw == 3))
        {
            m_held_arrow = -1;                   // arrow released: cancel auto-repeat
        }
        else if (zone == 0x0D && press)          // cursor cluster + wheel-mode keys (hardware-verified)
        {
            switch (sw)
            {
                // arrows: fire immediately and arm auto-repeat in Run()
                case 4: CSurf_OnArrow(0, m_arrow_mode == 1); m_held_arrow = 0; m_arrow_held_since = timeGetTime(); break; // up
                case 0: CSurf_OnArrow(1, m_arrow_mode == 1); m_held_arrow = 1; m_arrow_held_since = timeGetTime(); break; // down
                case 1: // left: scroll/zoom or bank-scroll+mixer-scroll
                    if (m_arrow_mode == 2)
                    {
                        AdjustBankOffset(-1); m_held_arrow = 2; m_arrow_held_since = timeGetTime();
                        if (SetMixerScroll) { MediaTrack *t = CSurf_TrackFromID(m_bank_offset + 1, false); if (t) SetMixerScroll(t); }
                    }
                    else { CSurf_OnArrow(2, m_arrow_mode == 1); m_held_arrow = 2; m_arrow_held_since = timeGetTime(); }
                    break;
                case 3: // right: scroll/zoom or bank-scroll+mixer-scroll
                    if (m_arrow_mode == 2)
                    {
                        AdjustBankOffset(1); m_held_arrow = 3; m_arrow_held_since = timeGetTime();
                        if (SetMixerScroll) { MediaTrack *t = CSurf_TrackFromID(m_bank_offset + 1, false); if (t) SetMixerScroll(t); }
                    }
                    else { CSurf_OnArrow(3, m_arrow_mode == 1); m_held_arrow = 3; m_arrow_held_since = timeGetTime(); }
                    break;
                case 2:                                        // INC -> next marker
                    SendMessage(g_hwnd, WM_COMMAND, ID_MARKER_NEXT, 0);
                    break;
                case 5:                                        // SCRUB: wheel scrubs audio
                    m_wheel_mode = (m_wheel_mode == 1) ? 0 : 1;
                    SendGlobalLED(0x0D, 5, m_wheel_mode == 1);
                    SendGlobalLED(0x0D, 6, false);
                    break;
                case 6:                                        // SHUTTLE: wheel moves coarse
                    m_wheel_mode = (m_wheel_mode == 2) ? 0 : 2;
                    SendGlobalLED(0x0D, 6, m_wheel_mode == 2);
                    SendGlobalLED(0x0D, 5, false);
                    break;
            }
        }
        else if (zone == 0x13 && press && sw != 5) // LOCATE MEMORY 1-6 (hw-verified; sw5 is companion event)
        {
            // non-sequential sw: LM1=sw1 LM2=sw3 LM3=sw6 LM4=sw2 LM5=sw4 LM6=sw7
            static const int lm_idx[] = { -1, 0, 3, 1, 4, -1, 2, 5 }; // index by sw -> m_la_lm index
            if (sw < 8 && lm_idx[sw] >= 0 && m_la_lm[lm_idx[sw]])
                SendMessage(g_hwnd, WM_COMMAND, m_la_lm[lm_idx[sw]], 0);
        }
        else if (zone == 0x15 && press && (sw == 0 || sw == 1)) // LM7/LM8 (hw-captured 2026-06-15)
        {
            if (m_la_lm[6 + sw])
                SendMessage(g_hwnd, WM_COMMAND, m_la_lm[6 + sw], 0);
        }
        else if (zone == 0x1B && sw == 7 && press) // DEC -> previous marker
        {
            SendMessage(g_hwnd, WM_COMMAND, ID_MARKER_PREV, 0);
        }
        else if (zone == 0x14 && sw == 0 && press) // ENTER: cycle cursor arrows scroll -> zoom -> bank-scroll
        {
            m_arrow_mode = (m_arrow_mode + 1) % 3;
        }
        else if (zone == 0x19 && sw == 2 && press && port == 0) // AUTOMIX ENABLE (hw-verified; all 3 ports)
        {
            int newMode = (m_auto_mode == 0) ? 1 : 0; // toggle bypass <-> read
            CSurf_SetAutoMode(newMode, this);
            m_auto_mode = newMode;
            SendAutomixLEDs(newMode);
        }
        else if (zone == 0x0C && sw == 2 && press && port == 0) // AUTOMIX REC/WRITE (hw-verified)
        {
            CSurf_SetAutoMode(3, this);
            m_auto_mode = 3;
            SendAutomixLEDs(3);
        }
        else if (zone == 0x18 && press && port == 0) // AUTOMIX modes (hw-verified; all 3 ports; dedup via port 0)
        {
            // sw0=touch, sw1=return/read, sw2=relative/latch-preview, sw4=abort/undo, sw5=auto-rec/latch
            int newMode = -1;
            switch (sw)
            {
                case 0: newMode = 2; break; // touch sense -> touch
                case 1: newMode = 1; break; // return -> read
                case 2: newMode = 5; break; // relative -> latch preview
                case 4: SendMessage(g_hwnd, WM_COMMAND, IDC_EDIT_UNDO, 0); break; // abort/undo -> undo
                case 5: newMode = 4; break; // auto-rec -> latch
            }
            if (newMode >= 0)
            {
                CSurf_SetAutoMode(newMode, this);
                m_auto_mode = newMode;
                SendAutomixLEDs(newMode);
            }
        }
    }

    void AdjustBankOffset(int amt)
    {
        int maxoffs = CSurf_NumTracks(false) - 1;
        if (maxoffs < 0) maxoffs = 0;

        int offs = m_bank_offset + amt;
        if (offs < 0) offs = 0;
        else if (offs > maxoffs) offs = maxoffs;
        if (offs == m_bank_offset) return;

        m_bank_offset = offs;
        m_held_arrow = -1;
        memset(m_vol_lastpos, 0xff, sizeof(m_vol_lastpos));
        memset(m_pan_lastpos, 0xff, sizeof(m_pan_lastpos));
        memset(m_meter_lastlvl, 0xff, sizeof(m_meter_lastlvl));
        memset(m_meter_hist, 0, sizeof(m_meter_hist));
        memset(m_fader_touch, 0, sizeof(m_fader_touch));
        TrackList_UpdateAllExternalSurfaces(); // repaint faders/LEDs for the new bank
        SetTrackListChange();                  // blank scribbles on channels past the last track
    }
};

static void GetIniPath(char *buf, int bufsz)
{
    buf[0] = '\0';
#ifdef _WIN32
    // prefer REAPER's own GetResourcePath (not in the csurf SDK headers, so look it up at runtime)
    typedef const char *(*GRP_t)();
    HMODULE hReaper = GetModuleHandleA("reaper.exe");
    GRP_t fn = hReaper ? (GRP_t)GetProcAddress(hReaper, "GetResourcePath") : NULL;
    if (fn)
        sprintf_s(buf, bufsz, "%s\\dm2000_keys.ini", fn());
    else
    {
        char appdata[MAX_PATH] = "";
        SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata);
        sprintf_s(buf, bufsz, "%s\\REAPER\\dm2000_keys.ini", appdata);
    }
#else
    // macOS: GetResourcePath (resolved via rec->GetFunc in csurf_main) returns
    // ~/Library/Application Support/REAPER; the keys file lives alongside reaper.ini.
    // No GetModuleHandle on SWELL, so this is the only portable resolver.
    if (GetResourcePath)
        sprintf_s(buf, bufsz, "%s/dm2000_keys.ini", GetResourcePath());
#endif
}

static void parseParms(const char *str, int parms[8], char *iniPath, int iniPathSz)
{
    for (int i = 0; i < 8; ++i) parms[i] = -1;
    if (iniPath) iniPath[0] = '\0';

    const char *p = str;
    if (p)
    {
        int x = 0;
        while (x < 8 && *p)
        {
            while (*p == ' ') p++;
            if (*p == '|' || ((*p < '0' || *p > '9') && *p != '-')) break;
            parms[x++] = atoi(p);
            while (*p && *p != ' ' && *p != '|') p++;
        }
        if (*p == '|' && iniPath)
            sprintf_s(iniPath, iniPathSz, "%s", p + 1);
    }
}

static IReaperControlSurface *createFunc(const char *type_string, const char *configString, int *errStats)
{
    int parms[8];
    char iniPath[MAX_PATH] = "";
    parseParms(configString, parms, iniPath, sizeof(iniPath));

    return new CSurf_DM2000(parms[0], parms[1], parms[2], parms[3], parms[4], parms[5], parms[6], parms[7], iniPath, errStats);
}

static WDL_DLGRET dlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            int parms[8];
            char iniPath[MAX_PATH] = "";
            parseParms((const char *)lParam, parms, iniPath, sizeof(iniPath));
            if (!iniPath[0]) GetIniPath(iniPath, sizeof(iniPath));

            // port group combo: consecutive 4-port groups from MIDI inputs
            int n = GetNumMIDIInputs();
            int x = SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_ADDSTRING, 0, (LPARAM)"None");
            SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_SETITEMDATA, x, -1);
            SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_SETCURSEL, x, 0);
            for (int j = 0; j + 3 < n; ++j)
            {
                char first[256], last[256];
                if (GetMIDIInputName(j, first, sizeof(first)) && GetMIDIInputName(j + 3, last, sizeof(last)))
                {
                    char buf[600];
                    sprintf(buf, "%s ... %s", first, last);
                    int a = SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_ADDSTRING, 0, (LPARAM)buf);
                    SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_SETITEMDATA, a, j);
                    if (j == parms[0]) SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_SETCURSEL, a, 0);
                }
            }

            SetDlgItemTextA(hwndDlg, IDC_EDIT_INIPATH, iniPath);
#ifndef _WIN32
            {
                // Set link text color to NSColor.linkColor (system blue, dark-mode aware).
                // WM_CTLCOLORSTATIC in SWELL applies one color to all statics, so we use
                // objc_msgSend directly to target only the two link controls.
                typedef id   (*MSId)(id, SEL);
                typedef void (*MSVoidId)(id, SEL, id);
                id cls = (id)objc_getClass("NSColor");
                id col = ((MSId)objc_msgSend)(cls, sel_registerName("linkColor"));
                SEL setCol = sel_registerName("setTextColor:");
                id c1 = (id)GetDlgItem(hwndDlg, IDC_DM2000_LINK);
                id c2 = (id)GetDlgItem(hwndDlg, IDC_DM2000_INIEXAMPLE);
                if (c1) ((MSVoidId)objc_msgSend)(c1, setCol, col);
                if (c2) ((MSVoidId)objc_msgSend)(c2, setCol, col);
            }
#endif
        }
        break;
        case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            if (id == IDC_BTN_OPENFOLDER)
            {
                char iniPath[MAX_PATH];
                GetDlgItemTextA(hwndDlg, IDC_EDIT_INIPATH, iniPath, sizeof(iniPath));
                char folder[MAX_PATH];
                strcpy_s(folder, sizeof(folder), iniPath);
                char *sep = strrchr(folder, DM2000_PATHSEP);
                if (sep) *sep = '\0';
#ifdef _WIN32
                ShellExecuteA(hwndDlg, "explore", folder, NULL, NULL, SW_SHOWNORMAL);
#else
                // SWELL routes a bare directory path to NSWorkspace openFile (opens in Finder).
                ShellExecute(hwndDlg, "open", folder, NULL, NULL, 0);
#endif
            }
        }
        break;
#ifndef _WIN32
        case WM_MOUSEMOVE:
        {
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            auto overLink = [&](int ctrlId) -> bool {
                HWND h = GetDlgItem(hwndDlg, ctrlId);
                RECT rc; GetWindowRect(h, &rc);
                ScreenToClient(hwndDlg, (LPPOINT)&rc);
                ScreenToClient(hwndDlg, (LPPOINT)&rc + 1);
                return !!PtInRect(&rc, pt);
            };
            SetCursor(LoadCursor(NULL,
                (overLink(IDC_DM2000_LINK) || overLink(IDC_DM2000_INIEXAMPLE))
                ? IDC_HAND : IDC_ARROW));
        }
        break;
        case WM_LBUTTONDOWN:
        {
            // SWELL does not fire WM_COMMAND for SS_NOTIFY statics, so hit-test manually.
            POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
            HWND hwndLink = GetDlgItem(hwndDlg, IDC_DM2000_LINK);
            RECT rcLink;
            GetWindowRect(hwndLink, &rcLink);
            ScreenToClient(hwndDlg, (LPPOINT)&rcLink);
            ScreenToClient(hwndDlg, (LPPOINT)&rcLink + 1);
            if (PtInRect(&rcLink, pt))
            {
                ShellExecute(hwndDlg, "open", "https://bmroz.eu/projects/dm2000-csurf", NULL, NULL, 0);
                return 1;
            }
            HWND hwndIni = GetDlgItem(hwndDlg, IDC_DM2000_INIEXAMPLE);
            RECT rcIni;
            GetWindowRect(hwndIni, &rcIni);
            ScreenToClient(hwndDlg, (LPPOINT)&rcIni);
            ScreenToClient(hwndDlg, (LPPOINT)&rcIni + 1);
            if (PtInRect(&rcIni, pt))
            {
                ShellExecute(hwndDlg, "open", "https://github.com/mormegil6/reaper-dm2000-csurf/blob/main/doc/dm2000_keys.ini.example", NULL, NULL, 0);
                return 1;
            }
        }
        break;
#endif
        case WM_USER + 1024:
            if (wParam > 1 && lParam)
            {
                char tmp[MAX_PATH + 64];

                int indevs[4] = { -1, -1, -1, -1 }, outdevs[4] = { -1, -1, -1, -1 };
                int start = -1;
                int r = SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_GETCURSEL, 0, 0);
                if (r != CB_ERR) start = SendDlgItemMessage(hwndDlg, IDC_COMBO_PORTGROUP, CB_GETITEMDATA, r, 0);

                if (start >= 0)
                {
                    // output devices are enumerated separately from inputs, so the
                    // same physical port can have a different index in each list --
                    // locate the output port by name instead of reusing the input index
                    int outstart = start;
                    char inname[256];
                    if (GetMIDIInputName(start, inname, sizeof(inname)))
                    {
                        int nout = GetNumMIDIOutputs();
                        for (int j = 0; j < nout; ++j)
                        {
                            char outname[256];
                            if (GetMIDIOutputName(j, outname, sizeof(outname)) && !strcmp(outname, inname))
                            {
                                outstart = j;
                                break;
                            }
                        }
                    }

                    for (int i = 0; i < 4; ++i)
                    {
                        indevs[i] = start + i;
                        outdevs[i] = outstart + i;
                    }
                }

                char iniPath[MAX_PATH] = "";
                GetDlgItemTextA(hwndDlg, IDC_EDIT_INIPATH, iniPath, sizeof(iniPath));
                if (iniPath[0])
                    sprintf(tmp, "%d %d %d %d %d %d %d %d|%s", indevs[0], outdevs[0], indevs[1], outdevs[1], indevs[2], outdevs[2], indevs[3], outdevs[3], iniPath);
                else
                    sprintf(tmp, "%d %d %d %d %d %d %d %d", indevs[0], outdevs[0], indevs[1], outdevs[1], indevs[2], outdevs[2], indevs[3], outdevs[3]);
                lstrcpyn((char *)lParam, tmp, wParam);
            }
        break;
#ifdef _WIN32
        // SWELL has no WHITE_BRUSH and its GetStockObject only knows NULL_BRUSH/NULL_PEN,
        // so this would not compile on macOS; SWELL edit fields render white by default.
        case WM_CTLCOLOREDIT:
            if (GetDlgCtrlID((HWND)lParam) == IDC_EDIT_INIPATH)
            {
                SetBkColor((HDC)wParam, RGB(255, 255, 255));
                return (INT_PTR)GetStockObject(WHITE_BRUSH);
            }
        break;
        // SysLink + NMLINK exist only on Windows; on macOS the footer is an SS_NOTIFY
        // static handled via WM_COMMAND/IDC_DM2000_LINK above (NMLINK is undefined in SWELL).
        case WM_NOTIFY:
        {
            NMHDR *hdr = (NMHDR *)lParam;
            if (hdr->code == NM_CLICK || hdr->code == NM_RETURN)
            {
                NMLINK *link = (NMLINK *)lParam;
                ShellExecuteW(hwndDlg, L"open", link->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
            }
        }
        break;
#endif
    }
    return 0;
}

static HWND configFunc(const char *type_string, HWND parent, const char *initConfigString)
{
    return CreateDialogParam(g_hInst, MAKEINTRESOURCE(IDD_SURFACEEDIT_DM2000), parent, dlgProc, (LPARAM)initConfigString);
}

reaper_csurf_reg_t csurf_dm2000_reg =
{
  "DM2000",
  "Yamaha DM2000",
  createFunc,
  configFunc,
};
